/*
 * 暗房实现。几何约定（与取景一致）：
 *
 *   传感器装在机身里转了 90°：传感器画面逆时针转 90° 才是"取景方向"（film_viewfinder.c 同一规则；
 *   真机核对过：早先按顺时针转，画面是倒的）。
 *   转过之后的"取景方向"里，M6 全画幅占满整幅（竖幅）；4:3 裁切占满宽度、高度只取中间 9/16；
 *   宝丽来取中间的正方形。
 *   再按握持方向 rot 转正（规则与界面 ui_point 相同），得到用户看到的正立照片。
 *
 * 因为传感器的一行对应取景方向的一列，按条带读原片时，每条带能完成取景方向上的若干整列，
 * 所以不需要整幅原片常驻内存。
 *
 * 实现上，重采样按"顺时针转 90°"的方向逐列完成（传感器从上往下的行依次落到取景方向从右往左的列），
 * 写入时再多转 180°（column_target 用 rot + 180°）。两次旋转合起来就是逆时针 90° + 握持方向。
 */
#include "film_darkroom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "film_assets.h"
#include "film_yuv.h"

#define MAX_SOURCE_WIDTH    4096
/*
 * 重采样每次产出取景方向的一整列。横拿时一列落在成片里是跨行写（每个像素隔一整行），
 * 在 PSRAM 上几乎每次都缓存未命中；先攒 TILE_COLS 列再按行写出，一次写满一段连续字节。
 * 取 8：偶数，YUV420 存放时写入的矩形满足 2×2 色度块对齐。
 */
#define TILE_COLS           8
#define SCREEN_PRINT_H      FILM_DARKROOM_SCREEN_MAX_H
/* 屏幕尺寸相纸的比例参照（旧版 1140×1387，图像 1024，边距 58/78）；全分辨率相纸放大后，屏幕版式保持不变 */
#define SCREEN_PRINT_REF_W      1140
#define SCREEN_PRINT_REF_H      1387
#define SCREEN_PRINT_REF_SIDE   58
#define SCREEN_PRINT_REF_TOP    78
/* YUV420 成片分条带处理时每条带的行数（偶数） */
#define STRIP_ROWS          32
/* 存盘成片各边的对齐：硬件 JPEG 编码按 16×16 的 MCU 读取 */
#define FULL_ALIGN          16
/* 相纸边线向图像外侧最多伸出的像素数；YUV420 下按 2 的倍数取，保证条带矩形与色度块对齐 */
#define EDGE_PAD            2
/* 日期戳字号的参照：该短边的图像用原字号（M6 旧成片短边 1080，宝丽来旧图像 1024），更大的图像按比例放大 */
#define STAMP_REF_SHORT_M6      1080
#define STAMP_REF_SHORT_INSTANT 1024

/* 日期戳：橙色 LED 字，外加一圈光晕 */
#define STAMP_CORE_COLOR    0xFF9A3CU
#define STAMP_GLOW_COLOR    0xFF6A10U
#define STAMP_GLOW_ALPHA    190
#define STAMP_GLOW_PAD_DIV  20       /*!< 撇号光晕外扩 = 字号 / 20 */
/* 撇号（DSEG7 缺字）的尺寸，占数字高度的百分比 */
#define STAMP_TICK_W_PCT    11
#define STAMP_TICK_H_PCT    30
#define STAMP_TICK_ADV_PCT  30
#define STAMP_SLANT_PCT     10       /*!< 与 DSEG7 笔段相同的前倾 */
#define STAMP_TRACKING_EM   0.14f
#define STAMP_MARGIN_X      0.045f   /*!< 右边距占图像宽度 */
#define STAMP_MARGIN_Y      0.050f   /*!< 下边距占图像高度 */

/* 相纸：药膜凹边 + 纸张厚度高光；RGB565 下仍要看得见，但不能像硬描边。 */
#define PRINT_EDGE_ALPHA       56
#define PRINT_RECESS_ALPHA     22
#define PRINT_HIGHLIGHT_ALPHA  20

struct film_darkroom_t {
    film_darkroom_config_t config;
    int32_t col_x0[FILM_DARKROOM_MAX_EDGE];     /*!< 取景方向每一行 y 对应的传感器列（左邻） */
    uint8_t col_fx[FILM_DARKROOM_MAX_EDGE];     /*!< 与右邻的插值权重 Q8 */
    uint8_t balance_lut[3][256];                /*!< 原片校正查找表（每次冲洗重建） */
    film_tone_hist_t balance_hist;              /*!< 原片直方图（对齐小样时精确求解用） */
};

/** 一张成片的版式 */
typedef struct {
    int out_w, out_h;       /*!< 缓冲尺寸（相纸或照片） */
    int img_x, img_y;       /*!< 图像在缓冲里的位置 */
    int img_w, img_h;       /*!< 转正后的图像尺寸 */
    int scr_w, scr_h;       /*!< 取景方向（转正之前）的图像尺寸 */
    bool print;
} layout_t;

/** 成片缓冲：RGB888（整幅直接读写）或 YUV420（只能经 film_yuv 按矩形读写） */
typedef struct {
    film_image_format_t format;
    uint8_t *data;
    int width, height;
    size_t stride;          /*!< 仅 RGB888 */
} store_t;

static void report(const film_darkroom_job_t *job, film_stage_t stage, float done)
{
    if (job->progress) {
        job->progress(job->progress_ctx, stage, done);
    }
}

static bool rot_swaps(gfx_rot_t rot)
{
    return rot == GFX_ROT_90 || rot == GFX_ROT_270;
}

static int floor16(int v)
{
    return v & ~(FULL_ALIGN - 1);
}

/** 存盘成片的基准边（取景方向的图像宽），见 film_darkroom_job_t.full_edge */
static int full_edge(const film_darkroom_job_t *job)
{
    if (job->full_edge == 0 || job->full_edge >= FILM_DARKROOM_M6_W) {
        return FILM_DARKROOM_M6_W;
    }
    const int edge = floor16(job->full_edge);
    return edge >= FULL_ALIGN ? edge : FULL_ALIGN;
}

/** 按基准边缩放一个标准尺寸（以 FILM_DARKROOM_M6_W 为基准），向下取 16 的倍数；基准边为标准时结果就是原值 */
static int full_scaled(int standard, int edge)
{
    return floor16(standard * edge / FILM_DARKROOM_M6_W);
}

/** M6 在取景方向（转正之前）的图像尺寸：全画幅为 3:4 竖幅，4:3 裁切为横幅；屏幕图与取景画面同样的取景范围 */
static void m6_scr_size(const film_darkroom_job_t *job, int *ret_w, int *ret_h)
{
    if (job->size != FILM_DARKROOM_FULL) {
        *ret_w = job->crop_43 ? FILM_DARKROOM_SCREEN_MAX_W : FILM_DARKROOM_SCREEN_MAX_H;
        *ret_h = job->crop_43 ? FILM_DARKROOM_SCREEN_MAX_H : FILM_DARKROOM_SCREEN_MAX_W;
        return;
    }
    const int edge = full_edge(job);
    *ret_w = edge;
    *ret_h = full_scaled(job->crop_43 ? FILM_DARKROOM_M6_H : FILM_DARKROOM_M6_FULL_H, edge);
}

static void make_layout(const film_darkroom_job_t *job, layout_t *l)
{
    memset(l, 0, sizeof(*l));
    const bool screen = job->size == FILM_DARKROOM_SCREEN;
    if (job->size == FILM_DARKROOM_VIEWFINDER) {
        if (job->instant) {
            l->scr_w = FILM_DARKROOM_SCREEN_MAX_H;
            l->scr_h = FILM_DARKROOM_SCREEN_MAX_H;
        } else {
            m6_scr_size(job, &l->scr_w, &l->scr_h);
        }
        l->img_w = l->out_w = l->scr_w;
        l->img_h = l->out_h = l->scr_h;
        return;
    }
    if (job->instant) {
        l->print = true;
        if (screen) {
            const float k = (float)SCREEN_PRINT_H / SCREEN_PRINT_REF_H;
            l->out_w = (int)lroundf(SCREEN_PRINT_REF_W * k);
            l->out_h = SCREEN_PRINT_H;
            l->img_x = (int)lroundf(SCREEN_PRINT_REF_SIDE * k);
            l->img_y = (int)lroundf(SCREEN_PRINT_REF_TOP * k);
            l->img_w = l->out_w - 2 * l->img_x;
        } else {
            /* 标准基准边时正是 1712×2096、图像 1536、边距 88/118；更小的原片按比例缩小，位置取偶数 */
            const int edge = full_edge(job);
            l->out_w = full_scaled(FILM_DARKROOM_PRINT_W, edge);
            l->out_h = full_scaled(FILM_DARKROOM_PRINT_H, edge);
            l->img_w = full_scaled(FILM_DARKROOM_PRINT_IMAGE, edge);
            l->img_x = ((l->out_w - l->img_w) / 2) & ~1;
            l->img_y = (FILM_DARKROOM_PRINT_TOP * edge / FILM_DARKROOM_M6_W) & ~1;
        }
        l->img_h = l->img_w;
        l->scr_w = l->scr_h = l->img_w;
        return;
    }
    m6_scr_size(job, &l->scr_w, &l->scr_h);
    l->img_w = rot_swaps(job->rot) ? l->scr_h : l->scr_w;
    l->img_h = rot_swaps(job->rot) ? l->scr_w : l->scr_h;
    l->out_w = l->img_w;
    l->out_h = l->img_h;
}

void film_darkroom_output_size(const film_darkroom_job_t *job, int *ret_w, int *ret_h)
{
    layout_t l;
    make_layout(job, &l);
    *ret_w = l.out_w;
    *ret_h = l.out_h;
}

void film_darkroom_fit(int width, int height, int max_w, int max_h, int *ret_w, int *ret_h)
{
    int w = max_w;
    int h = (int)((int64_t)height * max_w / width);
    if (h > max_h) {
        h = max_h;
        w = (int)((int64_t)width * max_h / height);
    }
    *ret_w = w > 0 ? w : 1;
    *ret_h = h > 0 ? h : 1;
}

/* ---------------------------------------------------------------- 重采样 */

/** 取景方向一列（x）写到成片里的起点和逐 y 的步进 */
static void column_target(const layout_t *l, gfx_rot_t rot, int x, uint8_t *pixels, size_t stride,
                          uint8_t **ret_ptr, ptrdiff_t *ret_step)
{
    int u, v;
    ptrdiff_t step;
    switch (rot) {
    case GFX_ROT_90:
        u = 0;
        v = l->scr_w - 1 - x;
        step = 3;
        break;
    case GFX_ROT_180:
        u = l->scr_w - 1 - x;
        v = l->scr_h - 1;
        step = -(ptrdiff_t)stride;
        break;
    case GFX_ROT_270:
        u = l->scr_h - 1;
        v = x;
        step = -3;
        break;
    default:
        u = x;
        v = 0;
        step = (ptrdiff_t)stride;
        break;
    }
    *ret_ptr = pixels + (size_t)(l->img_y + v) * stride + (size_t)(l->img_x + u) * 3;
    *ret_step = step;
}

typedef struct {
    film_darkroom_handle_t dr;
    const layout_t *layout;
    gfx_rot_t rot;
    const store_t *store;
    float scale;            /*!< 取景方向 1 像素 = 多少传感器像素 */
    bool exact;             /*!< 1:1 且落在整数像素上：直接拷贝，不插值 */
    int crop_x0;            /*!< 裁切块在传感器里的起始列（exact 路径直接取用） */
    int block_h;            /*!< 传感器行数（裁切块高度） */
    int next_x;             /*!< 下一列待处理的取景列（从右往左） */
    /* 当前可用的传感器行：carry（上一条带最后一行）+ 本条带 [r0, r1) */
    const uint8_t *strip;
    size_t strip_stride;
    int r0, r1;
    uint8_t *carry;
    int carry_row;
    /* 列缓冲：第 i 列（取景列 tile_x0 - i）的第 y 个像素在 tile[(y * TILE_COLS + i) * 3] */
    uint8_t *tile;
    uint8_t *rect;          /*!< YUV420 存放时，把列缓冲转成成片方向的矩形，再整块写入 */
    int tile_x0;
    int tile_n;
    esp_err_t err;
} resample_t;

/** 把攒好的列写进 RGB888 成片，按目标内存连续的方向遍历 */
static void flush_tile_rgb(resample_t *r)
{
    const int scr_h = r->layout->scr_h;
    const int n = r->tile_n;
    uint8_t *dst[TILE_COLS];
    ptrdiff_t step = 0;
    for (int i = 0; i < n; ++i) {
        column_target(r->layout, r->rot, r->tile_x0 - i, r->store->data, r->store->stride, &dst[i], &step);
    }
    if (step == 3 || step == -3) {
        /* 竖拿：一列本身就是成片里连续的一行 */
        for (int i = 0; i < n; ++i) {
            const uint8_t *src = r->tile + (size_t)i * 3;
            uint8_t *d = dst[i];
            for (int y = 0; y < scr_h; ++y, src += TILE_COLS * 3, d += step) {
                d[0] = src[0];
                d[1] = src[1];
                d[2] = src[2];
            }
        }
    } else {
        /* 横拿：同一个 y 上相邻的几列在成片里相邻 */
        for (int y = 0; y < scr_h; ++y) {
            const uint8_t *src = r->tile + (size_t)y * TILE_COLS * 3;
            const ptrdiff_t row = (ptrdiff_t)y * step;
            for (int i = 0; i < n; ++i, src += 3) {
                uint8_t *d = dst[i] + row;
                d[0] = src[0];
                d[1] = src[1];
                d[2] = src[2];
            }
        }
    }
}

/**
 * 把攒好的列写进 YUV420 成片：先转成成片方向的 RGB 矩形（与 column_target 同一套朝向规则），
 * 矩形的位置和大小都是偶数（tile 从列 0 起每 8 列一块，成片的图像位置也是偶数），再整块转 YUV 写入。
 */
static void flush_tile_yuv(resample_t *r)
{
    const layout_t *l = r->layout;
    const int n = r->tile_n;
    const int scr_h = l->scr_h;
    const bool rows_mode = r->rot == GFX_ROT_90 || r->rot == GFX_ROT_270;     /*!< 一列落成一行 */
    const int rw = rows_mode ? scr_h : n;
    const int rh = rows_mode ? n : scr_h;
    const size_t rs = (size_t)rw * 3;
    int rx = 0, ry = 0;
    switch (r->rot) {
    case GFX_ROT_90:
        ry = l->scr_w - 1 - r->tile_x0;
        break;
    case GFX_ROT_270:
        ry = r->tile_x0 - (n - 1);
        break;
    case GFX_ROT_180:
        rx = l->scr_w - 1 - r->tile_x0;
        break;
    default:
        rx = r->tile_x0 - (n - 1);
        break;
    }
    for (int i = 0; i < n; ++i) {
        const uint8_t *src = r->tile + (size_t)i * 3;
        for (int y = 0; y < scr_h; ++y, src += TILE_COLS * 3) {
            int cx, cy;
            switch (r->rot) {
            case GFX_ROT_90:
                cx = y, cy = i;
                break;
            case GFX_ROT_270:
                cx = scr_h - 1 - y, cy = n - 1 - i;
                break;
            case GFX_ROT_180:
                cx = i, cy = scr_h - 1 - y;
                break;
            default:
                cx = n - 1 - i, cy = y;
                break;
            }
            uint8_t *d = r->rect + (size_t)cy * rs + (size_t)cx * 3;
            d[0] = src[0];
            d[1] = src[1];
            d[2] = src[2];
        }
    }
    if (film_yuv_put_rgb(r->store->data, (uint16_t)r->store->width, (uint16_t)r->store->height, l->img_x + rx,
                         l->img_y + ry, rw, rh, r->rect, rs) != 0) {
        r->err = ESP_ERR_INVALID_SIZE;
    }
}

static void flush_tile(resample_t *r)
{
    if (r->tile_n == 0) {
        return;
    }
    if (r->store->format == FILM_IMAGE_YUV420) {
        flush_tile_yuv(r);
    } else {
        flush_tile_rgb(r);
    }
    r->tile_n = 0;
}

static const uint8_t *row_at(const resample_t *r, int y)
{
    if (y == r->carry_row) {
        return r->carry;
    }
    return r->strip + (size_t)(y - r->r0) * r->strip_stride;
}

/** 处理所有所需传感器行都已到手的取景列 */
static void resample_columns(resample_t *r, bool final)
{
    const int scr_h = r->layout->scr_h;
    while (r->next_x >= 0 && r->err == ESP_OK) {
        int y0, y1, fy = 0;
        if (r->exact) {
            /* 1:1：取景列 x 就是传感器第 block_h - 1 - x 行，不需要邻行 */
            y0 = y1 = r->block_h - 1 - r->next_x;
        } else {
            const float sy = (float)(r->block_h - 1) - (((float)r->next_x + 0.5f) * r->scale - 0.5f);
            y0 = (int)floorf(sy);
            fy = (int)lroundf((sy - (float)y0) * 256.0f);
            if (y0 < 0) {
                y0 = 0;
                fy = 0;
            }
            y1 = y0 + 1 < r->block_h ? y0 + 1 : r->block_h - 1;
        }
        if (!final && y1 >= r->r1) {
            return;
        }
        if (y0 < r->r0 && y0 != r->carry_row) {
            y0 = y1;   /* 理论上不会发生：条带至少比缩放比例高 */
        }
        const uint8_t *a = row_at(r, y0);
        const uint8_t *b = row_at(r, y1);
        if (r->tile_n == 0) {
            r->tile_x0 = r->next_x;
        }
        uint8_t *dst = r->tile + (size_t)r->tile_n * 3;
        if (r->exact) {
            const uint8_t *px = a + (size_t)r->crop_x0 * 3;
            for (int y = 0; y < scr_h; ++y, px += 3, dst += TILE_COLS * 3) {
                dst[0] = px[0];
                dst[1] = px[1];
                dst[2] = px[2];
            }
        } else {
            for (int y = 0; y < scr_h; ++y) {
                const int32_t x0 = r->dr->col_x0[y];
                const int fx = r->dr->col_fx[y];
                const uint8_t *a0 = a + x0 * 3;
                const uint8_t *b0 = b + x0 * 3;
                for (int ch = 0; ch < 3; ++ch) {
                    const int top = a0[ch] * (256 - fx) + a0[ch + 3] * fx;
                    const int bottom = b0[ch] * (256 - fx) + b0[ch + 3] * fx;
                    dst[ch] = (uint8_t)((top * (256 - fy) + bottom * fy + 32768) >> 16);
                }
                dst += TILE_COLS * 3;
            }
        }
        --r->next_x;
        if (++r->tile_n == TILE_COLS) {
            flush_tile(r);
        }
    }
    flush_tile(r);
}

static esp_err_t resample(film_darkroom_handle_t dr, const film_darkroom_job_t *job, const layout_t *l,
                          const film_darkroom_source_t *src, const store_t *store)
{
    if (src->width < 16 || src->height < 16 || src->width > MAX_SOURCE_WIDTH || src->width < src->height) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 裁切块：传感器全部行 × 中间若干列（M6 全画幅取全部列）；转到取景方向后宽 = 行数 */
    const int block_h = src->height;
    int block_w = src->width;
    if (job->instant) {
        block_w = src->height;
    } else if (job->crop_43) {
        block_w = src->width * FILM_DARKROOM_M6_CROP_NUM / FILM_DARKROOM_M6_CROP_DEN;
    }
    const int crop_x0 = (src->width - block_w) / 2;
    const float scale = (float)block_h / (float)l->scr_w;
    const bool exact = block_h == l->scr_w && crop_x0 + l->scr_h <= src->width;

    /* 取景方向的 y 对应传感器的列，每条带都一样，先算好 */
    for (int y = 0; y < l->scr_h; ++y) {
        const float sx = (float)crop_x0 + ((float)y + 0.5f) * scale - 0.5f;
        int x0 = (int)floorf(sx);
        int fx = (int)lroundf((sx - (float)x0) * 256.0f);
        if (x0 < 0) {
            x0 = 0;
            fx = 0;
        }
        if (x0 >= src->width - 1) {
            x0 = src->width - 2;
            fx = 256;
        }
        dr->col_x0[y] = x0;
        dr->col_fx[y] = (uint8_t)(fx > 255 ? 255 : fx);
    }

    /* 握持方向再加 180°：把"顺时针 90°"的重采样方向改正成传感器实际的安装方向（见文件头） */
    const gfx_rot_t hold = job->size == FILM_DARKROOM_VIEWFINDER ? GFX_ROT_0 : job->rot;
    resample_t r = {
        .dr = dr, .layout = l, .rot = (gfx_rot_t)((hold + GFX_ROT_180) % 4), .store = store,
        .scale = scale, .exact = exact, .crop_x0 = crop_x0,
        .block_h = block_h, .next_x = l->scr_w - 1, .r0 = 0, .r1 = 0, .carry_row = -1,
    };
    const size_t tile_bytes = (size_t)l->scr_h * TILE_COLS * 3;
    r.carry = malloc((size_t)src->width * 3);
    r.tile = dr->config.alloc(tile_bytes, dr->config.alloc_ctx);
    if (store->format == FILM_IMAGE_YUV420) {
        r.rect = dr->config.alloc(tile_bytes, dr->config.alloc_ctx);
    }
    if (!r.carry || !r.tile || (store->format == FILM_IMAGE_YUV420 && !r.rect)) {
        free(r.carry);
        if (r.tile) {
            dr->config.free(r.tile, dr->config.alloc_ctx);
        }
        if (r.rect) {
            dr->config.free(r.rect, dr->config.alloc_ctx);
        }
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    for (;;) {
        /* 上一条带的最后一行留作 carry，供跨条带插值；必须在 read 之前拷：数据源可以复用条带缓冲 */
        if (r.r1 > r.r0) {
            memcpy(r.carry, r.strip + (size_t)(r.r1 - 1 - r.r0) * r.strip_stride, (size_t)src->width * 3);
            r.carry_row = r.r1 - 1;
        }
        const uint8_t *rows = NULL;
        size_t rows_stride = 0;
        int count = 0;
        err = src->read(src->ctx, &rows, &rows_stride, &count);
        if (err != ESP_OK || count <= 0) {
            break;
        }
        r.strip = rows;
        r.strip_stride = rows_stride;
        r.r0 = r.r1;
        r.r1 = r.r0 + count;
        resample_columns(&r, r.r1 >= block_h);
        if (r.err != ESP_OK) {
            err = r.err;
            break;
        }
        report(job, FILM_STAGE_READ, (float)r.r1 / (float)block_h);
        if (r.r1 >= block_h) {
            break;
        }
    }
    if (err == ESP_OK && r.next_x >= 0) {
        err = ESP_ERR_INVALID_SIZE;   /* 原片行数不够 */
    }
    dr->config.free(r.tile, dr->config.alloc_ctx);
    if (r.rect) {
        dr->config.free(r.rect, dr->config.alloc_ctx);
    }
    free(r.carry);
    return err;
}

/* ---------------------------------------------------------------- 相纸与日期戳 */

static void rgb565_to_888(uint16_t c, uint8_t *out)
{
    const uint8_t r = (uint8_t)((c >> 11) & 0x1F);
    const uint8_t g = (uint8_t)((c >> 5) & 0x3F);
    const uint8_t b = (uint8_t)(c & 0x1F);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 2));
}

/** 相纸第 y0 起的 rows 行铺纸纹（图像区随后被覆盖）；pixels 指向 y0 行的第 0 列 */
static void fill_paper_rows(const layout_t *l, uint8_t *pixels, size_t stride, int y0, int rows)
{
    const gfx_image_t *tex = &img_tex_paper;
    for (int j = 0; j < rows; ++j) {
        const int y = y0 + j;
        uint8_t *row = pixels + (size_t)j * stride;
        if (!tex->pixels) {
            for (int x = 0; x < l->out_w; ++x) {
                row[x * 3 + 0] = 0xF3;
                row[x * 3 + 1] = 0xEE;
                row[x * 3 + 2] = 0xE3;
            }
            continue;
        }
        const uint16_t *trow = tex->pixels + (size_t)(y % tex->height) * tex->width;
        for (int x = 0; x < l->out_w; ++x) {
            rgb565_to_888(trow[x % tex->width], row + x * 3);
        }
    }
}

static void darken_px(uint8_t *p, int alpha)
{
    for (int ch = 0; ch < 3; ++ch) {
        p[ch] = (uint8_t)(p[ch] * (255 - alpha) / 255);
    }
}

static void lighten_px(uint8_t *p, int alpha)
{
    for (int ch = 0; ch < 3; ++ch) {
        p[ch] = (uint8_t)(p[ch] + (255 - p[ch]) * alpha / 255);
    }
}

/** 一块 RGB888 矩形：pixels 是成片坐标 (x, y) 处的像素，只允许改矩形内的像素 */
typedef struct {
    uint8_t *pixels;
    size_t stride;
    int x, y, w, h;
} rect_t;

static uint8_t *rect_px(const rect_t *rc, int x, int y)
{
    if (x < rc->x || x >= rc->x + rc->w || y < rc->y || y >= rc->y + rc->h) {
        return NULL;
    }
    return rc->pixels + (size_t)(y - rc->y) * rc->stride + (size_t)(x - rc->x) * 3;
}

/**
 * 图像外侧药膜边、内侧凹槽和下/右侧纸张高光，让照片真正嵌进相纸。
 * 只改落在 rc 内的像素；先做横边再做竖边，同一像素的先后顺序与整幅处理一致，所以按条带分次调用结果不变。
 */
static void print_edge(const layout_t *l, const rect_t *rc)
{
    const int x_lo = l->img_x - 1;
    const int x_hi = l->img_x + l->img_w;      /*!< 含 */
    const struct {
        int y;
        int alpha;
        bool light;
    } rows[] = {
        { l->img_y - 1, PRINT_EDGE_ALPHA, false },
        { l->img_y + l->img_h, PRINT_EDGE_ALPHA / 2, false },
        { l->img_y, PRINT_RECESS_ALPHA, false },
        { l->img_y + l->img_h + 1, PRINT_HIGHLIGHT_ALPHA, true },
    };
    /* 原顺序是每个 x 依次处理四行；四行互不重叠，改成逐行处理等价 */
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (rows[i].y < rc->y || rows[i].y >= rc->y + rc->h) {
            continue;
        }
        for (int x = x_lo; x <= x_hi; ++x) {
            uint8_t *p = rect_px(rc, x, rows[i].y);
            if (p) {
                rows[i].light ? lighten_px(p, rows[i].alpha) : darken_px(p, rows[i].alpha);
            }
        }
    }
    const int y_lo = rc->y > l->img_y ? rc->y : l->img_y;
    const int y_hi = rc->y + rc->h < l->img_y + l->img_h ? rc->y + rc->h : l->img_y + l->img_h;   /*!< 不含 */
    for (int y = y_lo; y < y_hi; ++y) {
        uint8_t *p;
        if ((p = rect_px(rc, l->img_x - 1, y)) != NULL) {
            darken_px(p, PRINT_EDGE_ALPHA);
        }
        if ((p = rect_px(rc, l->img_x + l->img_w, y)) != NULL) {
            darken_px(p, PRINT_EDGE_ALPHA);
        }
        if ((p = rect_px(rc, l->img_x, y)) != NULL) {
            darken_px(p, PRINT_RECESS_ALPHA);
        }
        if ((p = rect_px(rc, l->img_x + l->img_w + 1, y)) != NULL) {
            lighten_px(p, PRINT_HIGHLIGHT_ALPHA);
        }
    }
}

typedef enum {
    BLEND_NORMAL,
    BLEND_SCREEN,   /*!< 发光：只会变亮 */
} blend_t;

static void blend_px(uint8_t *d, const uint8_t c[3], int a, blend_t blend)
{
    for (int ch = 0; ch < 3; ++ch) {
        if (blend == BLEND_SCREEN) {
            d[ch] = (uint8_t)(255 - (255 - d[ch]) * (255 - c[ch] * a / 255) / 255);
        } else {
            d[ch] = (uint8_t)(d[ch] + (c[ch] - d[ch]) * a / 255);
        }
    }
}

/** 日期戳的画布：图像第 y0 起的 rows 行（整幅处理时是整张图像）；pixels 指向 y0 行的第 0 列 */
typedef struct {
    uint8_t *pixels;
    size_t stride;
    int w;
    int y0, rows;
} canvas_t;

static uint8_t *canvas_px(const canvas_t *cv, int x, int y)
{
    if (x < 0 || x >= cv->w || y < cv->y0 || y >= cv->y0 + cv->rows) {
        return NULL;
    }
    return cv->pixels + (size_t)(y - cv->y0) * cv->stride + (size_t)x * 3;
}

/** 日期戳的放大：Q8，256 为原字号 */
static int stamp_scale(int v, int k_q8)
{
    return (v * k_q8 + 128) >> 8;
}

/** 数字的高度（像素，原字号）：以 '0' 的字形为准 */
static int digit_height(const gfx_font_t *font)
{
    const gfx_glyph_t *g = gfx_font_find(font, '0');
    return g && g->top < 0 ? -g->top : (font->ascent > 0 ? font->ascent : 1);
}

/** DSEG7 没有撇号：画一道和数码管笔段同样倾斜的短竖 */
static void draw_tick(const gfx_font_t *font, int k_q8, int pen_x, int baseline, int pad, const uint8_t c[3],
                      int alpha, blend_t blend, const canvas_t *cv)
{
    const int em = stamp_scale(digit_height(font), k_q8);
    const int w = em * STAMP_TICK_W_PCT / 100 + 2 * pad;
    const int h = em * STAMP_TICK_H_PCT / 100 + 2 * pad;
    const int top = baseline - em;
    for (int j = 0; j < h; ++j) {
        const int py = top + j;
        const int shift = (h - j) * STAMP_SLANT_PCT / 100;
        for (int i = 0; i < w; ++i) {
            uint8_t *d = canvas_px(cv, pen_x - pad + shift + i, py);
            if (d) {
                blend_px(d, c, alpha, blend);
            }
        }
    }
}

static int tick_advance(const gfx_font_t *font, int k_q8)
{
    return stamp_scale(digit_height(font), k_q8) * STAMP_TICK_ADV_PCT / 100;
}

/** 字形位图按放大倍数取一点的覆盖度（0..255）；256 倍时就是原位图，其余双线性 */
static int glyph_alpha(const uint8_t *bm, int gw, int gh, int i, int j, int k_q8)
{
    if (k_q8 == 256) {
        return bm[j * gw + i];
    }
    int sx = (((2 * i + 1) << 15) / k_q8) - 128;      /*!< 源坐标 Q8：(i + 0.5) / k - 0.5 */
    int sy = (((2 * j + 1) << 15) / k_q8) - 128;
    sx = sx < 0 ? 0 : sx;
    sy = sy < 0 ? 0 : sy;
    int x0 = sx >> 8, y0 = sy >> 8;
    int fx = sx & 255, fy = sy & 255;
    if (x0 >= gw - 1) {
        x0 = gw - 1;
        fx = 0;
    }
    if (y0 >= gh - 1) {
        y0 = gh - 1;
        fy = 0;
    }
    const int x1 = fx ? x0 + 1 : x0, y1 = fy ? y0 + 1 : y0;
    const int top = bm[y0 * gw + x0] * (256 - fx) + bm[y0 * gw + x1] * fx;
    const int bottom = bm[y1 * gw + x0] * (256 - fx) + bm[y1 * gw + x1] * fx;
    return (top * (256 - fy) + bottom * fy + 32768) >> 16;
}

static void draw_glyphs(const gfx_font_t *font, int k_q8, const char *text, int x, int baseline, int tracking, int pad,
                        uint32_t color, int alpha, blend_t blend, const canvas_t *cv)
{
    const uint8_t c[3] = { (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color };
    int pen_q4 = x * 16;
    uint32_t cp;
    const char *s = text;
    while (*s && (s = gfx_utf8_next(s, &cp)) != NULL) {
        if (cp == '\'') {
            draw_tick(font, k_q8, pen_q4 / 16, baseline, pad, c, alpha, blend, cv);
            pen_q4 += (tick_advance(font, k_q8) + tracking) * 16;
            continue;
        }
        const gfx_glyph_t *g = gfx_font_find(font, cp);
        if (!g) {
            continue;
        }
        const int gx = pen_q4 / 16 + stamp_scale(g->left, k_q8);
        const int gy = baseline + stamp_scale(g->top, k_q8);
        const int dw = k_q8 == 256 ? g->width : (g->width * k_q8 + 255) / 256;
        const int dh = k_q8 == 256 ? g->height : (g->height * k_q8 + 255) / 256;
        const uint8_t *bm = font->bitmap + g->offset;
        for (int j = 0; j < dh; ++j) {
            const int py = gy + j;
            if (py < cv->y0 || py >= cv->y0 + cv->rows) {
                continue;
            }
            for (int i = 0; i < dw; ++i) {
                const int a = glyph_alpha(bm, g->width, g->height, i, j, k_q8) * alpha / 255;
                uint8_t *d = a > 0 ? canvas_px(cv, gx + i, py) : NULL;
                if (d) {
                    blend_px(d, c, a, blend);
                }
            }
        }
        pen_q4 += (int)(((int64_t)g->advance_q4 * k_q8 + 128) >> 8) + tracking * 16;
    }
}

static int text_width(const gfx_font_t *font, int k_q8, const char *text, int tracking)
{
    int w_q4 = 0;
    int n = 0;
    uint32_t cp;
    const char *s = text;
    while (*s && (s = gfx_utf8_next(s, &cp)) != NULL) {
        const gfx_glyph_t *g = cp == '\'' ? NULL : gfx_font_find(font, cp);
        if (cp == '\'') {
            w_q4 += tick_advance(font, k_q8) * 16;
            ++n;
        } else if (g) {
            w_q4 += (int)(((int64_t)g->advance_q4 * k_q8 + 128) >> 8);
            ++n;
        }
    }
    return w_q4 / 16 + (n > 1 ? (n - 1) * tracking : 0);
}

/** 在画布（图像的若干行）上压日期戳；字号、位置只取决于整幅图像，所以按条带分次调用结果不变 */
static void draw_stamp(const film_darkroom_job_t *job, const layout_t *l, const canvas_t *cv)
{
    const bool full = job->size == FILM_DARKROOM_FULL;
    const gfx_font_t *core = full ? &font_dseg_60 : &font_dseg_20;
    const gfx_font_t *glow = full ? &font_dseg_60_glow : &font_dseg_20_glow;
    if (!core->bitmap || !glow->bitmap) {
        return;
    }
    /* 全分辨率成片的图像比旧版大，字号按短边放大，日期戳占画面的比例不变 */
    const int short_edge = l->img_w < l->img_h ? l->img_w : l->img_h;
    const int ref = job->instant ? STAMP_REF_SHORT_INSTANT : STAMP_REF_SHORT_M6;
    /* 小原片（full_edge）的成片也按短边缩小字号，否则日期戳占画面的比例会变大 */
    const bool scaled = full && (short_edge > ref || full_edge(job) < FILM_DARKROOM_M6_W);
    const int k_q8 = scaled ? short_edge * 256 / ref : 256;
    const int size_px = stamp_scale(full ? 60 : 20, k_q8);
    const int tracking = (int)lroundf(size_px * STAMP_TRACKING_EM);
    const int w = text_width(core, k_q8, job->stamp, tracking);
    const int x = l->img_w - (int)lroundf(l->img_w * STAMP_MARGIN_X) - w;
    const int baseline = l->img_h - (int)lroundf(l->img_h * STAMP_MARGIN_Y);
    const int glow_pad = size_px / STAMP_GLOW_PAD_DIV;
    draw_glyphs(glow, k_q8, job->stamp, x, baseline, tracking, glow_pad, STAMP_GLOW_COLOR, STAMP_GLOW_ALPHA,
                BLEND_SCREEN, cv);
    draw_glyphs(core, k_q8, job->stamp, x, baseline, tracking, 0, STAMP_CORE_COLOR, 235, BLEND_NORMAL, cv);
}

/* ---------------------------------------------------------------- 公共接口 */

esp_err_t film_darkroom_create(const film_darkroom_config_t *config, film_darkroom_handle_t *ret_handle)
{
    if (!config || !config->filter || !config->alloc || !config->free || !ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    film_darkroom_handle_t dr = calloc(1, sizeof(*dr));
    if (!dr) {
        return ESP_ERR_NO_MEM;
    }
    dr->config = *config;
    *ret_handle = dr;
    return ESP_OK;
}

void film_darkroom_delete(film_darkroom_handle_t handle)
{
    free(handle);
}

/** 由统计结果和作业参数定下白平衡校正编码，并填好 ret_stats / ret_balance；stats 为空表示没有测量 */
static film_balance_t resolve_balance(film_darkroom_handle_t dr, const film_darkroom_job_t *job,
                                      const film_tone_stats_t *stats)
{
    film_balance_t code = job->balance;
    if (stats) {
        if (job->match) {
            code = film_balance_match(stats, &dr->balance_hist, job->match);
        }
        if (job->ret_stats) {
            *job->ret_stats = *stats;
        }
    }
    if (job->ret_balance) {
        *job->ret_balance = code;
    }
    if (code != FILM_BALANCE_NONE) {
        film_balance_build_lut(code, dr->balance_lut);
    }
    return code;
}

static film_develop_params_t film_params(const film_darkroom_job_t *job)
{
    return (film_develop_params_t) {
        .film = job->film,
        .seed = job->seed,
        .light_leak = job->light_leak,
        .grain = true,
        .exposure_ev = job->exposure_ev + FILM_DARKROOM_PRINT_EV,
    };
}

/* ---------------------------------------------------------------- RGB888 成片：整幅处理 */

static esp_err_t finish_rgb(film_darkroom_handle_t dr, const film_darkroom_job_t *job, const layout_t *l,
                            const store_t *st)
{
    uint8_t *img = st->data + (size_t)l->img_y * st->stride + (size_t)l->img_x * 3;
    film_tone_stats_t stats;
    const bool measured = job->match || job->ret_stats;
    if (measured) {
        film_balance_measure(img, st->stride, l->img_w, l->img_h, &stats, job->match ? &dr->balance_hist : NULL);
    }
    const film_balance_t code = resolve_balance(dr, job, measured ? &stats : NULL);
    if (code != FILM_BALANCE_NONE) {
        film_balance_apply(img, st->stride, l->img_w, l->img_h, dr->balance_lut);
    }
    esp_err_t err = ESP_OK;
    if (job->film < FILM_ID_COUNT) {
        report(job, FILM_STAGE_FILTER, 0.0f);
        const film_develop_params_t params = film_params(job);
        const film_image_t image = {
            .pixels = img, .width = (uint16_t)l->img_w, .height = (uint16_t)l->img_h, .stride = st->stride,
            .order = FILM_ORDER_RGB,
        };
        err = film_filter_develop(dr->config.filter, &params, &image);
        report(job, FILM_STAGE_FINISH, 0.0f);
        if (err == ESP_OK && job->stamp && job->stamp[0]) {
            const canvas_t cv = { .pixels = img, .stride = st->stride, .w = l->img_w, .y0 = 0, .rows = l->img_h };
            draw_stamp(job, l, &cv);
        }
    }
    if (err == ESP_OK && l->print) {
        const rect_t whole = { .pixels = st->data, .stride = st->stride, .x = 0, .y = 0, .w = l->out_w, .h = l->out_h };
        print_edge(l, &whole);
    }
    return err;
}

/* ---------------------------------------------------------------- YUV420 成片：分条带处理 */

/** 把成片的一个矩形读成 RGB 条带（存进 rgb） */
static esp_err_t strip_get(const store_t *st, uint8_t *rgb, int x, int y, int w, int h)
{
    return film_yuv_get_rgb(st->data, (uint16_t)st->width, (uint16_t)st->height, x, y, w, h, rgb, (size_t)w * 3) == 0
               ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t strip_put(const store_t *st, const uint8_t *rgb, int x, int y, int w, int h)
{
    return film_yuv_put_rgb(st->data, (uint16_t)st->width, (uint16_t)st->height, x, y, w, h, rgb, (size_t)w * 3) == 0
               ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t fill_paper_strips(const layout_t *l, const store_t *st, uint8_t *rgb)
{
    for (int y = 0; y < l->out_h; y += STRIP_ROWS) {
        const int n = l->out_h - y < STRIP_ROWS ? l->out_h - y : STRIP_ROWS;
        fill_paper_rows(l, rgb, (size_t)l->out_w * 3, y, n);
        const esp_err_t err = strip_put(st, rgb, 0, y, l->out_w, n);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/** 成片的 YUV420 缓冲经条带完成白平衡、滤镜、日期戳、相纸边；rgb 是条带暂存（STRIP_ROWS × 最宽矩形） */
static esp_err_t finish_yuv(film_darkroom_handle_t dr, const film_darkroom_job_t *job, const layout_t *l,
                            const store_t *st, uint8_t *rgb)
{
    const bool has_film = job->film < FILM_ID_COUNT;
    const int pad = l->print ? EDGE_PAD : 0;            /*!< 条带矩形比图像左右各宽 pad 列，容纳相纸边线 */
    const int rect_x = l->img_x - pad;
    const int rect_w = l->img_w + 2 * pad;
    const size_t rect_stride = (size_t)rect_w * 3;
    esp_err_t err = ESP_OK;

    /* 白平衡：统计与整幅一致（抽样点按绝对行号取），校正量求出后查表在条带里应用 */
    film_tone_stats_t stats;
    const bool measured = job->match || job->ret_stats;
    if (measured) {
        film_tone_accum_t acc;
        film_balance_accum_begin(&acc, job->match ? &dr->balance_hist : NULL);
        for (int s = 0; s < l->img_h && err == ESP_OK; s += STRIP_ROWS) {
            const int n = l->img_h - s < STRIP_ROWS ? l->img_h - s : STRIP_ROWS;
            err = strip_get(st, rgb, l->img_x, l->img_y + s, l->img_w, n);
            if (err == ESP_OK) {
                film_balance_accum_rows(&acc, rgb, (size_t)l->img_w * 3, l->img_w, s, n);
            }
        }
        if (err != ESP_OK) {
            return err;
        }
        film_balance_accum_finish(&acc, &stats);
    }
    const film_balance_t code = resolve_balance(dr, job, measured ? &stats : NULL);
    const bool lut = code != FILM_BALANCE_NONE;
    const bool stamp = has_film && job->stamp && job->stamp[0];
    if (!has_film && !lut && !l->print) {
        return ESP_OK;
    }

    report(job, FILM_STAGE_FILTER, 0.0f);
    film_strip_pre_pass_t pre = FILM_STRIP_PRE_PASS_NONE;
    if (has_film) {
        const film_develop_params_t params = film_params(job);
        err = film_filter_strip_begin(dr->config.filter, &params, (uint16_t)l->img_w, (uint16_t)l->img_h,
                                      FILM_ORDER_RGB);
        if (err != ESP_OK) {
            return err;
        }
        pre = film_filter_strip_pre_pass(dr->config.filter);
    }

    /* 第一遍（红晕胶卷调色并累计高光；像素风只统计格子颜色）：此时条带里的白平衡也要已经生效 */
    if (pre != FILM_STRIP_PRE_PASS_NONE) {
        for (int s = 0; s < l->img_h && err == ESP_OK; s += STRIP_ROWS) {
            const int n = l->img_h - s < STRIP_ROWS ? l->img_h - s : STRIP_ROWS;
            err = strip_get(st, rgb, l->img_x, l->img_y + s, l->img_w, n);
            if (err != ESP_OK) {
                break;
            }
            if (lut) {
                film_balance_apply(rgb, (size_t)l->img_w * 3, l->img_w, n, dr->balance_lut);
            }
            const film_image_t strip = {
                .pixels = rgb, .width = (uint16_t)l->img_w, .height = (uint16_t)n, .stride = (size_t)l->img_w * 3,
                .order = FILM_ORDER_RGB,
            };
            err = film_filter_strip_pre(dr->config.filter, &strip, (uint16_t)s);
            if (err == ESP_OK && pre == FILM_STRIP_PRE_PASS_IN_PLACE) {
                err = strip_put(st, rgb, l->img_x, l->img_y + s, l->img_w, n);
            }
            report(job, FILM_STAGE_FILTER, 0.5f * (float)(s + n) / (float)l->img_h);
        }
        if (err == ESP_OK) {
            err = film_filter_strip_resolve(dr->config.filter);
        }
    }
    const bool lut_done = pre == FILM_STRIP_PRE_PASS_IN_PLACE;      /*!< 调色后的像素已含白平衡 */

    /* 最后一遍：白平衡（未生效时）、滤镜收尾、日期戳、相纸边 */
    const float base = pre != FILM_STRIP_PRE_PASS_NONE ? 0.5f : 0.0f;
    for (int s = 0; s < l->img_h && err == ESP_OK; s += STRIP_ROWS) {
        const int n = l->img_h - s < STRIP_ROWS ? l->img_h - s : STRIP_ROWS;
        err = strip_get(st, rgb, rect_x, l->img_y + s, rect_w, n);
        if (err != ESP_OK) {
            break;
        }
        uint8_t *img = rgb + (size_t)pad * 3;
        if (lut && !lut_done) {
            film_balance_apply(img, rect_stride, l->img_w, n, dr->balance_lut);
        }
        if (has_film) {
            const film_image_t strip = {
                .pixels = img, .width = (uint16_t)l->img_w, .height = (uint16_t)n, .stride = rect_stride,
                .order = FILM_ORDER_RGB,
            };
            err = film_filter_strip_finish(dr->config.filter, &strip, (uint16_t)s);
            if (err != ESP_OK) {
                break;
            }
            if (stamp) {
                const canvas_t cv = { .pixels = img, .stride = rect_stride, .w = l->img_w, .y0 = s, .rows = n };
                draw_stamp(job, l, &cv);
            }
        }
        if (l->print) {
            const rect_t rc = { .pixels = rgb, .stride = rect_stride, .x = rect_x, .y = l->img_y + s, .w = rect_w, .h = n };
            print_edge(l, &rc);
        }
        err = strip_put(st, rgb, rect_x, l->img_y + s, rect_w, n);
        report(job, FILM_STAGE_FILTER, base + (1.0f - base) * (float)(s + n) / (float)l->img_h);
    }
    if (has_film) {
        film_filter_strip_end(dr->config.filter);
    }
    report(job, FILM_STAGE_FINISH, 0.0f);

    /* 相纸边线在图像上下各伸出的几行（图像条带之外），上下各一个矩形 */
    if (err == ESP_OK && l->print) {
        const int bands[2] = { l->img_y - EDGE_PAD, l->img_y + l->img_h };
        for (int i = 0; i < 2 && err == ESP_OK; ++i) {
            err = strip_get(st, rgb, rect_x, bands[i], rect_w, EDGE_PAD);
            if (err != ESP_OK) {
                break;
            }
            const rect_t rc = { .pixels = rgb, .stride = rect_stride, .x = rect_x, .y = bands[i], .w = rect_w, .h = EDGE_PAD };
            print_edge(l, &rc);
            err = strip_put(st, rgb, rect_x, bands[i], rect_w, EDGE_PAD);
        }
    }
    return err;
}

esp_err_t film_darkroom_develop(film_darkroom_handle_t dr, const film_darkroom_job_t *job,
                                const film_darkroom_source_t *source, film_darkroom_image_t *ret_image)
{
    if (!dr || !job || !source || !source->read || !ret_image || job->film > FILM_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    layout_t l;
    make_layout(job, &l);
    const bool yuv = job->size == FILM_DARKROOM_FULL && dr->config.full_format == FILM_IMAGE_YUV420;
    if (yuv && ((l.out_w | l.out_h | l.img_x | l.img_y | l.img_w | l.img_h) & 1)) {
        return ESP_ERR_INVALID_SIZE;        /* YUV420 的色度块要求版式的所有边界落在偶数坐标上 */
    }
    store_t st = {
        .format = yuv ? FILM_IMAGE_YUV420 : FILM_IMAGE_RGB888, .width = l.out_w, .height = l.out_h,
        .stride = yuv ? 0 : (size_t)l.out_w * 3,
    };
    const size_t size = yuv ? film_yuv_bytes((uint16_t)l.out_w, (uint16_t)l.out_h) : st.stride * (size_t)l.out_h;
    st.data = dr->config.alloc(size, dr->config.alloc_ctx);
    if (!st.data) {
        return ESP_ERR_NO_MEM;
    }
    /* YUV420 的条带暂存：最宽的矩形是整张相纸（铺纸纹）或图像加边线 */
    uint8_t *rgb = NULL;
    if (yuv) {
        rgb = dr->config.alloc((size_t)(l.out_w > l.img_w + 2 * EDGE_PAD ? l.out_w : l.img_w + 2 * EDGE_PAD) * 3 *
                                   STRIP_ROWS, dr->config.alloc_ctx);
        if (!rgb) {
            dr->config.free(st.data, dr->config.alloc_ctx);
            return ESP_ERR_NO_MEM;
        }
    }
    report(job, FILM_STAGE_READ, 0.0f);
    esp_err_t err = ESP_OK;
    if (l.print) {
        if (yuv) {
            err = fill_paper_strips(&l, &st, rgb);
        } else {
            fill_paper_rows(&l, st.data, st.stride, 0, l.out_h);
        }
    }
    if (err == ESP_OK) {
        err = resample(dr, job, &l, source, &st);
    }
    if (err == ESP_OK) {
        err = yuv ? finish_yuv(dr, job, &l, &st, rgb) : finish_rgb(dr, job, &l, &st);
    }
    if (rgb) {
        dr->config.free(rgb, dr->config.alloc_ctx);
    }
    if (err != ESP_OK) {
        dr->config.free(st.data, dr->config.alloc_ctx);
        return err;
    }
    *ret_image = (film_darkroom_image_t) {
        .pixels = st.data, .width = (uint16_t)l.out_w, .height = (uint16_t)l.out_h, .stride = st.stride, .size = size,
        .format = st.format,
    };
    return ESP_OK;
}

void film_darkroom_release(film_darkroom_handle_t dr, film_darkroom_image_t *image)
{
    if (dr && image && image->pixels) {
        dr->config.free(image->pixels, dr->config.alloc_ctx);
        image->pixels = NULL;
    }
}

int film_darkroom_image_get_rgb(const film_darkroom_image_t *image, int x, int y, int w, int h, uint8_t *rgb,
                                size_t rgb_stride)
{
    if (!image || !image->pixels || !rgb) {
        return -1;
    }
    if (image->format == FILM_IMAGE_YUV420) {
        return film_yuv_get_rgb(image->pixels, image->width, image->height, x, y, w, h, rgb, rgb_stride);
    }
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > image->width || y + h > image->height) {
        return -1;
    }
    for (int j = 0; j < h; ++j) {
        memcpy(rgb + (size_t)j * rgb_stride, image->pixels + (size_t)(y + j) * image->stride + (size_t)x * 3,
               (size_t)w * 3);
    }
    return 0;
}

void film_darkroom_downscale_565(const film_darkroom_image_t *src, uint16_t *dst, int dst_w, int dst_h,
                                 uint32_t *acc)
{
    const int sw = src->width;
    const int sh = src->height;
    /* YUV420 成片：逐行转成 RGB 再累加 */
    uint8_t *row_buf = NULL;
    if (src->format == FILM_IMAGE_YUV420) {
        row_buf = malloc((size_t)sw * 3);
        if (!row_buf) {
            memset(dst, 0, sizeof(uint16_t) * (size_t)dst_w * (size_t)dst_h);
            return;
        }
    }
    for (int dy = 0; dy < dst_h; ++dy) {
        const int sy0 = dy * sh / dst_h;
        int sy1 = (dy + 1) * sh / dst_h;
        sy1 = sy1 > sy0 ? sy1 : sy0 + 1;
        memset(acc, 0, sizeof(uint32_t) * 3 * (size_t)dst_w);
        for (int sy = sy0; sy < sy1; ++sy) {
            const uint8_t *row = src->pixels + (size_t)sy * src->stride;
            if (row_buf) {
                film_yuv_get_rgb(src->pixels, src->width, src->height, 0, sy, sw, 1, row_buf, (size_t)sw * 3);
                row = row_buf;
            }
            for (int dx = 0; dx < dst_w; ++dx) {
                const int sx0 = dx * sw / dst_w;
                int sx1 = (dx + 1) * sw / dst_w;
                sx1 = sx1 > sx0 ? sx1 : sx0 + 1;
                uint32_t r = 0, g = 0, b = 0;
                for (int sx = sx0; sx < sx1; ++sx) {
                    r += row[sx * 3];
                    g += row[sx * 3 + 1];
                    b += row[sx * 3 + 2];
                }
                acc[dx * 3] += r;
                acc[dx * 3 + 1] += g;
                acc[dx * 3 + 2] += b;
            }
        }
        for (int dx = 0; dx < dst_w; ++dx) {
            const int sx0 = dx * sw / dst_w;
            int sx1 = (dx + 1) * sw / dst_w;
            sx1 = sx1 > sx0 ? sx1 : sx0 + 1;
            const uint32_t n = (uint32_t)((sx1 - sx0) * (sy1 - sy0));
            const uint32_t r = acc[dx * 3] / n;
            const uint32_t g = acc[dx * 3 + 1] / n;
            const uint32_t b = acc[dx * 3 + 2] / n;
            dst[(size_t)dy * dst_w + dx] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
    free(row_buf);
}

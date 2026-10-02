/*
 * 暗房实现。几何约定（与取景一致）：
 *
 *   传感器装在机身里转了 90°：传感器画面逆时针转 90° 才是"取景方向"（film_viewfinder.c 同一规则；
 *   真机核对过：早先按顺时针转，画面是倒的）。
 *   转过之后的"取景方向"里，横幅 M6 取景框占满宽度、高度只取中间 9/16；宝丽来取中间的正方形。
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

#define MAX_SOURCE_WIDTH    4096
/*
 * 重采样每次产出取景方向的一整列。横拿时一列落在成片里是跨行写（每个像素隔一整行），
 * 在 PSRAM 上几乎每次都缓存未命中；先攒 TILE_COLS 列再按行写出，一次写满一段连续字节。
 */
#define TILE_COLS           8
#define SCREEN_PRINT_H      FILM_DARKROOM_SCREEN_MAX_H

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
    int32_t col_x0[FILM_DARKROOM_M6_W];     /*!< 取景方向每一行 y 对应的传感器列（左邻） */
    uint8_t col_fx[FILM_DARKROOM_M6_W];     /*!< 与右邻的插值权重 Q8 */
    uint8_t balance_lut[3][256];            /*!< 原片校正查找表（每次冲洗重建） */
    film_tone_hist_t balance_hist;          /*!< 原片直方图（对齐小样时精确求解用） */
};

/** 一张成片的版式 */
typedef struct {
    int out_w, out_h;       /*!< 缓冲尺寸（相纸或照片） */
    int img_x, img_y;       /*!< 图像在缓冲里的位置 */
    int img_w, img_h;       /*!< 转正后的图像尺寸 */
    int scr_w, scr_h;       /*!< 取景方向（转正之前）的图像尺寸 */
    bool print;
} layout_t;

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

static void make_layout(const film_darkroom_job_t *job, layout_t *l)
{
    memset(l, 0, sizeof(*l));
    const bool screen = job->size == FILM_DARKROOM_SCREEN;
    if (job->size == FILM_DARKROOM_VIEWFINDER) {
        l->scr_w = l->img_w = l->out_w = job->instant ? FILM_DARKROOM_SCREEN_MAX_H : FILM_DARKROOM_SCREEN_MAX_W;
        l->scr_h = l->img_h = l->out_h = FILM_DARKROOM_SCREEN_MAX_H;
        return;
    }
    if (job->instant) {
        l->print = true;
        if (screen) {
            const float k = (float)SCREEN_PRINT_H / FILM_DARKROOM_PRINT_H;
            l->out_w = (int)lroundf(FILM_DARKROOM_PRINT_W * k);
            l->out_h = SCREEN_PRINT_H;
            l->img_x = (int)lroundf(FILM_DARKROOM_PRINT_SIDE * k);
            l->img_y = (int)lroundf(FILM_DARKROOM_PRINT_TOP * k);
            l->img_w = l->out_w - 2 * l->img_x;
        } else {
            l->out_w = FILM_DARKROOM_PRINT_W;
            l->out_h = FILM_DARKROOM_PRINT_H;
            l->img_x = FILM_DARKROOM_PRINT_SIDE;
            l->img_y = FILM_DARKROOM_PRINT_TOP;
            l->img_w = FILM_DARKROOM_PRINT_IMAGE;
        }
        l->img_h = l->img_w;
        l->scr_w = l->scr_h = l->img_w;
        return;
    }
    l->scr_w = screen ? FILM_DARKROOM_SCREEN_MAX_W : FILM_DARKROOM_M6_W;
    l->scr_h = screen ? FILM_DARKROOM_SCREEN_MAX_H : FILM_DARKROOM_M6_H;
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
    uint8_t *pixels;
    size_t stride;
    float scale;            /*!< 取景方向 1 像素 = 多少传感器像素 */
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
    int tile_x0;
    int tile_n;
} resample_t;

/** 把攒好的列写进成片，按目标内存连续的方向遍历 */
static void flush_tile(resample_t *r)
{
    if (r->tile_n == 0) {
        return;
    }
    const int scr_h = r->layout->scr_h;
    const int n = r->tile_n;
    uint8_t *dst[TILE_COLS];
    ptrdiff_t step = 0;
    for (int i = 0; i < n; ++i) {
        column_target(r->layout, r->rot, r->tile_x0 - i, r->pixels, r->stride, &dst[i], &step);
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
    while (r->next_x >= 0) {
        const float sy = (float)(r->block_h - 1) - (((float)r->next_x + 0.5f) * r->scale - 0.5f);
        int y0 = (int)floorf(sy);
        int fy = (int)lroundf((sy - (float)y0) * 256.0f);
        if (y0 < 0) {
            y0 = 0;
            fy = 0;
        }
        const int y1 = y0 + 1 < r->block_h ? y0 + 1 : r->block_h - 1;
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
        --r->next_x;
        if (++r->tile_n == TILE_COLS) {
            flush_tile(r);
        }
    }
    flush_tile(r);
}

static esp_err_t resample(film_darkroom_handle_t dr, const film_darkroom_job_t *job, const layout_t *l,
                          const film_darkroom_source_t *src, uint8_t *pixels, size_t stride)
{
    if (src->width < 16 || src->height < 16 || src->width > MAX_SOURCE_WIDTH || src->width < src->height) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* 裁切块：传感器全部行 × 中间若干列；转到取景方向后宽 = 行数 */
    const int block_h = src->height;
    const int block_w = job->instant ? src->height : src->width * FILM_DARKROOM_M6_CROP_NUM / FILM_DARKROOM_M6_CROP_DEN;
    const int crop_x0 = (src->width - block_w) / 2;
    const float scale = (float)block_h / (float)l->scr_w;

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
        .dr = dr, .layout = l, .rot = (gfx_rot_t)((hold + GFX_ROT_180) % 4), .pixels = pixels, .stride = stride,
        .scale = scale,
        .block_h = block_h, .next_x = l->scr_w - 1, .r0 = 0, .r1 = 0, .carry_row = -1,
    };
    r.carry = malloc((size_t)src->width * 3);
    r.tile = malloc((size_t)l->scr_h * TILE_COLS * 3);
    if (!r.carry || !r.tile) {
        free(r.carry);
        free(r.tile);
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
        report(job, FILM_STAGE_READ, (float)r.r1 / (float)block_h);
        if (r.r1 >= block_h) {
            break;
        }
    }
    if (err == ESP_OK && r.next_x >= 0) {
        err = ESP_ERR_INVALID_SIZE;   /* 原片行数不够 */
    }
    free(r.tile);
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

/** 整张相纸铺纸纹（图像区随后被覆盖） */
static void fill_paper(const layout_t *l, uint8_t *pixels, size_t stride)
{
    const gfx_image_t *tex = &img_tex_paper;
    for (int y = 0; y < l->out_h; ++y) {
        uint8_t *row = pixels + (size_t)y * stride;
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

/** 图像外侧药膜边、内侧凹槽和下/右侧纸张高光，让照片真正嵌进相纸。 */
static void print_edge(const layout_t *l, uint8_t *pixels, size_t stride)
{
    for (int x = l->img_x - 1; x <= l->img_x + l->img_w; ++x) {
        darken_px(pixels + (size_t)(l->img_y - 1) * stride + (size_t)x * 3, PRINT_EDGE_ALPHA);
        darken_px(pixels + (size_t)(l->img_y + l->img_h) * stride + (size_t)x * 3, PRINT_EDGE_ALPHA / 2);
        darken_px(pixels + (size_t)l->img_y * stride + (size_t)x * 3, PRINT_RECESS_ALPHA);
        lighten_px(pixels + (size_t)(l->img_y + l->img_h + 1) * stride + (size_t)x * 3, PRINT_HIGHLIGHT_ALPHA);
    }
    for (int y = l->img_y; y < l->img_y + l->img_h; ++y) {
        darken_px(pixels + (size_t)y * stride + (size_t)(l->img_x - 1) * 3, PRINT_EDGE_ALPHA);
        darken_px(pixels + (size_t)y * stride + (size_t)(l->img_x + l->img_w) * 3, PRINT_EDGE_ALPHA);
        darken_px(pixels + (size_t)y * stride + (size_t)l->img_x * 3, PRINT_RECESS_ALPHA);
        lighten_px(pixels + (size_t)y * stride + (size_t)(l->img_x + l->img_w + 1) * 3,
                   PRINT_HIGHLIGHT_ALPHA);
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

/** 数字的高度（像素）：以 '0' 的字形为准 */
static int digit_height(const gfx_font_t *font)
{
    const gfx_glyph_t *g = gfx_font_find(font, '0');
    return g && g->top < 0 ? -g->top : (font->ascent > 0 ? font->ascent : 1);
}

/** DSEG7 没有撇号：画一道和数码管笔段同样倾斜的短竖 */
static void draw_tick(const gfx_font_t *font, int pen_x, int baseline, int pad, const uint8_t c[3], int alpha,
                      blend_t blend, uint8_t *pixels, size_t stride, int clip_w, int clip_h)
{
    const int em = digit_height(font);
    const int w = em * STAMP_TICK_W_PCT / 100 + 2 * pad;
    const int h = em * STAMP_TICK_H_PCT / 100 + 2 * pad;
    const int top = baseline - em;
    for (int j = 0; j < h; ++j) {
        const int py = top + j;
        const int shift = (h - j) * STAMP_SLANT_PCT / 100;
        for (int i = 0; i < w; ++i) {
            const int px = pen_x - pad + shift + i;
            if (px >= 0 && px < clip_w && py >= 0 && py < clip_h) {
                blend_px(pixels + (size_t)py * stride + (size_t)px * 3, c, alpha, blend);
            }
        }
    }
}

static int tick_advance(const gfx_font_t *font)
{
    return digit_height(font) * STAMP_TICK_ADV_PCT / 100;
}

static void draw_glyphs(const gfx_font_t *font, const char *text, int x, int baseline, int tracking, int pad,
                        uint32_t color, int alpha, blend_t blend, uint8_t *pixels, size_t stride, int clip_w,
                        int clip_h)
{
    const uint8_t c[3] = { (uint8_t)(color >> 16), (uint8_t)(color >> 8), (uint8_t)color };
    int pen_q4 = x * 16;
    uint32_t cp;
    const char *s = text;
    while (*s && (s = gfx_utf8_next(s, &cp)) != NULL) {
        if (cp == '\'') {
            draw_tick(font, pen_q4 / 16, baseline, pad, c, alpha, blend, pixels, stride, clip_w, clip_h);
            pen_q4 += (tick_advance(font) + tracking) * 16;
            continue;
        }
        const gfx_glyph_t *g = gfx_font_find(font, cp);
        if (!g) {
            continue;
        }
        const int gx = pen_q4 / 16 + g->left;
        const int gy = baseline + g->top;
        const uint8_t *bm = font->bitmap + g->offset;
        for (int j = 0; j < g->height; ++j) {
            const int py = gy + j;
            if (py < 0 || py >= clip_h) {
                continue;
            }
            for (int i = 0; i < g->width; ++i) {
                const int px = gx + i;
                const int a = bm[j * g->width + i] * alpha / 255;
                if (px >= 0 && px < clip_w && a > 0) {
                    blend_px(pixels + (size_t)py * stride + (size_t)px * 3, c, a, blend);
                }
            }
        }
        pen_q4 += g->advance_q4 + tracking * 16;
    }
}

static int text_width(const gfx_font_t *font, const char *text, int tracking)
{
    int w_q4 = 0;
    int n = 0;
    uint32_t cp;
    const char *s = text;
    while (*s && (s = gfx_utf8_next(s, &cp)) != NULL) {
        const gfx_glyph_t *g = cp == '\'' ? NULL : gfx_font_find(font, cp);
        if (cp == '\'') {
            w_q4 += tick_advance(font) * 16;
            ++n;
        } else if (g) {
            w_q4 += g->advance_q4;
            ++n;
        }
    }
    return w_q4 / 16 + (n > 1 ? (n - 1) * tracking : 0);
}

static void draw_stamp(const film_darkroom_job_t *job, const layout_t *l, uint8_t *pixels, size_t stride)
{
    const bool full = job->size == FILM_DARKROOM_FULL;
    const gfx_font_t *core = full ? &font_dseg_60 : &font_dseg_20;
    const gfx_font_t *glow = full ? &font_dseg_60_glow : &font_dseg_20_glow;
    if (!core->bitmap || !glow->bitmap) {
        return;
    }
    const int size_px = full ? 60 : 20;
    const int tracking = (int)lroundf(size_px * STAMP_TRACKING_EM);
    const int w = text_width(core, job->stamp, tracking);
    const int x = l->img_w - (int)lroundf(l->img_w * STAMP_MARGIN_X) - w;
    const int baseline = l->img_h - (int)lroundf(l->img_h * STAMP_MARGIN_Y);
    uint8_t *img = pixels + (size_t)l->img_y * stride + (size_t)l->img_x * 3;
    const int glow_pad = size_px / STAMP_GLOW_PAD_DIV;
    draw_glyphs(glow, job->stamp, x, baseline, tracking, glow_pad, STAMP_GLOW_COLOR, STAMP_GLOW_ALPHA, BLEND_SCREEN, img,
                stride, l->img_w, l->img_h);
    draw_glyphs(core, job->stamp, x, baseline, tracking, 0, STAMP_CORE_COLOR, 235, BLEND_NORMAL, img, stride,
                l->img_w, l->img_h);
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

/** 统计原片均值，按 match 或 balance 求出校正并原地应用 */
static void balance_source(film_darkroom_handle_t dr, const film_darkroom_job_t *job, const layout_t *l,
                           uint8_t *pixels, size_t stride)
{
    uint8_t *img = pixels + (size_t)l->img_y * stride + (size_t)l->img_x * 3;
    film_balance_t code = job->balance;
    if (job->match || job->ret_stats) {
        film_tone_stats_t stats;
        film_tone_hist_t *hist = job->match ? &dr->balance_hist : NULL;
        film_balance_measure(img, stride, l->img_w, l->img_h, &stats, hist);
        if (job->match) {
            code = film_balance_match(&stats, hist, job->match);
        }
        if (job->ret_stats) {
            *job->ret_stats = stats;
        }
    }
    if (job->ret_balance) {
        *job->ret_balance = code;
    }
    if (code != FILM_BALANCE_NONE) {
        film_balance_build_lut(code, dr->balance_lut);
        film_balance_apply(img, stride, l->img_w, l->img_h, dr->balance_lut);
    }
}

esp_err_t film_darkroom_develop(film_darkroom_handle_t dr, const film_darkroom_job_t *job,
                                const film_darkroom_source_t *source, film_darkroom_image_t *ret_image)
{
    if (!dr || !job || !source || !source->read || !ret_image || job->film > FILM_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    layout_t l;
    make_layout(job, &l);
    const size_t stride = (size_t)l.out_w * 3;
    const size_t size = stride * (size_t)l.out_h;
    uint8_t *pixels = dr->config.alloc(size, dr->config.alloc_ctx);
    if (!pixels) {
        return ESP_ERR_NO_MEM;
    }
    if (l.print) {
        fill_paper(&l, pixels, stride);
    }
    report(job, FILM_STAGE_READ, 0.0f);
    esp_err_t err = resample(dr, job, &l, source, pixels, stride);
    if (err == ESP_OK) {
        balance_source(dr, job, &l, pixels, stride);
    }
    if (err == ESP_OK && job->film < FILM_ID_COUNT) {
        report(job, FILM_STAGE_FILTER, 0.0f);
        const film_develop_params_t params = {
            .film = job->film,
            .seed = job->seed,
            .light_leak = job->light_leak,
            .grain = true,
            .exposure_ev = job->exposure_ev + FILM_DARKROOM_PRINT_EV,
        };
        const film_image_t image = {
            .pixels = pixels + (size_t)l.img_y * stride + (size_t)l.img_x * 3,
            .width = (uint16_t)l.img_w,
            .height = (uint16_t)l.img_h,
            .stride = stride,
            .order = FILM_ORDER_RGB,
        };
        err = film_filter_develop(dr->config.filter, &params, &image);
        report(job, FILM_STAGE_FINISH, 0.0f);
        if (err == ESP_OK && job->stamp && job->stamp[0]) {
            draw_stamp(job, &l, pixels, stride);
        }
    }
    if (err == ESP_OK && l.print) {
        print_edge(&l, pixels, stride);
    }
    if (err != ESP_OK) {
        dr->config.free(pixels, dr->config.alloc_ctx);
        return err;
    }
    *ret_image = (film_darkroom_image_t) {
        .pixels = pixels, .width = (uint16_t)l.out_w, .height = (uint16_t)l.out_h, .stride = stride, .size = size,
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

void film_darkroom_downscale_565(const film_darkroom_image_t *src, uint16_t *dst, int dst_w, int dst_h,
                                 uint32_t *acc)
{
    const int sw = src->width;
    const int sh = src->height;
    for (int dy = 0; dy < dst_h; ++dy) {
        const int sy0 = dy * sh / dst_h;
        int sy1 = (dy + 1) * sh / dst_h;
        sy1 = sy1 > sy0 ? sy1 : sy0 + 1;
        memset(acc, 0, sizeof(uint32_t) * 3 * (size_t)dst_w);
        for (int sy = sy0; sy < sy1; ++sy) {
            const uint8_t *row = src->pixels + (size_t)sy * src->stride;
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
}

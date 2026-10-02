/*
 * 像素风胶卷：长边 96 格的均值下采样 → 提饱和/对比 → 4×4 Bayer 抖动
 * → 32 色调色板最近色 → 最近邻放大回原尺寸。
 *
 * 格子最多 96×96 个，这一段用浮点计算即可，逐像素的放大只做查表拷贝。
 */
#include "film_filter_private.h"

#define PIXEL_SATURATION    1.4f
#define PIXEL_CONTRAST      1.08f
#define PIXEL_PIVOT_IN      118.0f
#define PIXEL_PIVOT_OUT     128.0f
#define PIXEL_DITHER        18.0f   /*!< 抖动强度（0..255 色阶） */

/* Endesga 32 复古调色板，含肤色、粉紫和青绿过渡色，适合照片 */
static const uint8_t s_palette[FILM_PIXEL_PALETTE_SIZE][FILM_CHANNELS] = {
    { 0xbe, 0x4a, 0x2f }, { 0xd7, 0x76, 0x43 }, { 0xea, 0xd4, 0xaa }, { 0xe4, 0xa6, 0x72 },
    { 0xb8, 0x6f, 0x50 }, { 0x73, 0x3e, 0x39 }, { 0x3e, 0x27, 0x31 }, { 0xa2, 0x26, 0x33 },
    { 0xe4, 0x3b, 0x44 }, { 0xf7, 0x76, 0x22 }, { 0xfe, 0xae, 0x34 }, { 0xfe, 0xe7, 0x61 },
    { 0x63, 0xc7, 0x4d }, { 0x3e, 0x89, 0x48 }, { 0x26, 0x5c, 0x42 }, { 0x19, 0x3c, 0x3e },
    { 0x12, 0x4e, 0x89 }, { 0x00, 0x99, 0xdb }, { 0x2c, 0xe8, 0xf5 }, { 0xff, 0xff, 0xff },
    { 0xc0, 0xcb, 0xdc }, { 0x8b, 0x9b, 0xb4 }, { 0x5a, 0x69, 0x88 }, { 0x3a, 0x44, 0x66 },
    { 0x26, 0x2b, 0x44 }, { 0x18, 0x14, 0x25 }, { 0xff, 0x00, 0x44 }, { 0x68, 0x38, 0x6c },
    { 0xb5, 0x50, 0x88 }, { 0xf6, 0x75, 0x7a }, { 0xe8, 0xb7, 0x96 }, { 0xc2, 0x85, 0x69 },
};

/* 最近色距离的通道权重，近似人眼对 R/G/B 的敏感度 */
static const float s_channel_weight[FILM_CHANNELS] = { 2.0f, 4.0f, 3.0f };

/* 4×4 Bayer 矩阵（0..15），使用时换算为 (m + 0.5) / 16 - 0.5，即 -0.5..0.5 */
static const uint8_t s_bayer4[4][4] = {
    { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
};

static const float s_luma[FILM_CHANNELS] = { 0.299f, 0.587f, 0.114f };

static inline float clamp255(float v)
{
    return v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
}

/* 四舍五入的整数除法，用于格子数量换算 */
static inline uint16_t div_round(uint32_t num, uint32_t den)
{
    return (uint16_t)((num * 2 + den) / (den * 2));
}

/*
 * 第 i 格覆盖的源像素范围 [first, end)：像素中心 j + 0.5 落在 (i·s, (i+1)·s] 内即属于该格，
 * s = src_len / cells。这与 Pillow BOX 缩放的取样规则相同。
 */
static void cell_span(uint16_t i, uint16_t cells, uint16_t src_len, uint16_t *ret_first, uint16_t *ret_end)
{
    uint32_t n2 = 2u * cells;
    uint32_t first = (2u * i * src_len + cells) / n2;
    uint32_t end = (2u * (i + 1u) * src_len + cells) / n2;
    /* 格子比源像素还多（极小图）时，至少取一个像素且不越界 */
    first = first < src_len ? first : src_len - 1u;
    end = end > first ? end : first + 1u;
    *ret_first = (uint16_t)first;
    *ret_end = (uint16_t)(end < src_len ? end : src_len);
}

/*
 * 求一格的平均色：先逐行横向平均并取整到 8 位，再对这些行均值纵向平均，
 * 与 Pillow 先水平后垂直、中间结果存 8 位的两遍缩放保持一致。
 */
static void box_mean(const film_image_t *image, uint16_t x0, uint16_t x1, uint16_t y0, uint16_t y1,
                     float *ret_rgb)
{
    uint32_t cw = x1 - x0;
    uint32_t ch = y1 - y0;
    uint32_t col_sum[FILM_CHANNELS] = { 0 };
    for (uint16_t y = y0; y < y1; y++) {
        const uint8_t *px = image->pixels + (size_t)y * image->stride + (size_t)x0 * FILM_CHANNELS;
        uint32_t row_sum[FILM_CHANNELS] = { 0 };
        for (uint16_t x = x0; x < x1; x++, px += FILM_CHANNELS) {
            row_sum[0] += px[0];
            row_sum[1] += px[1];
            row_sum[2] += px[2];
        }
        for (int c = 0; c < FILM_CHANNELS; c++) {
            col_sum[c] += (row_sum[c] * 2 + cw) / (cw * 2);
        }
    }
    for (int c = 0; c < FILM_CHANNELS; c++) {
        ret_rgb[c] = (float)((col_sum[c] * 2 + ch) / (ch * 2));
    }
}

/* 一个格子的调色 + 抖动 + 量化，返回调色板下标 */
static int quantize_cell(float *rgb, uint16_t cx, uint16_t cy)
{
    float luma = rgb[0] * s_luma[0] + rgb[1] * s_luma[1] + rgb[2] * s_luma[2];
    float dither = (((float)s_bayer4[cy % 4][cx % 4] + 0.5f) / 16.0f - 0.5f) * PIXEL_DITHER;
    for (int c = 0; c < FILM_CHANNELS; c++) {
        float v = clamp255(luma + (rgb[c] - luma) * PIXEL_SATURATION);
        v = clamp255((v - PIXEL_PIVOT_IN) * PIXEL_CONTRAST + PIXEL_PIVOT_OUT);
        rgb[c] = v + dither;
    }

    int best = 0;
    float best_dist = 0.0f;
    for (int i = 0; i < FILM_PIXEL_PALETTE_SIZE; i++) {
        float dist = 0.0f;
        for (int c = 0; c < FILM_CHANNELS; c++) {
            float d = rgb[c] - s_palette[i][c];
            dist += d * d * s_channel_weight[c];
        }
        if (i == 0 || dist < best_dist) {
            best = i;
            best_dist = dist;
        }
    }
    return best;
}

/*
 * 冲洗分两遍：先把所有格子的颜色算完（按格子行切分），再放大回原图（按图像行切分）。
 * 放大区域与采样区域并不完全重合，不能边读边写，两遍之间必须等两段都完成。
 */
void film_pixel_prepare(film_develop_ctx_t *ctx)
{
    uint16_t w = ctx->image->width;
    uint16_t h = ctx->image->height;
    uint32_t longest = w > h ? w : h;
    /* 格子数与画面比例绑定，缩略图和大图的像素粒度一致 */
    uint16_t cols = div_round((uint32_t)w * FILM_PIXEL_COLUMNS, longest);
    uint16_t rows = div_round((uint32_t)h * FILM_PIXEL_COLUMNS, longest);
    ctx->pixel_cols = cols ? cols : 1;
    ctx->pixel_rows = rows ? rows : 1;

    /* 最近邻放大：像素中心 (x + 0.5) 落在哪个格子就取哪个格子；把列的换算预先做成表 */
    uint8_t *col_cell = ctx->filter->pixel_col_cell;
    for (uint16_t x = 0; x < w; x++) {
        col_cell[x] = (uint8_t)(((uint32_t)x * 2 + 1) * ctx->pixel_cols / (2u * w));
    }
}

void film_pixel_cell_rows(const film_develop_ctx_t *ctx, uint16_t row0, uint16_t row1)
{
    const film_image_t *image = ctx->image;
    const uint16_t cols = ctx->pixel_cols;
    const uint16_t rows = ctx->pixel_rows;
    uint8_t *cells = ctx->filter->pixel_cells;

    for (uint16_t cy = row0; cy < row1; cy++) {
        uint16_t y0, y1;
        cell_span(cy, rows, image->height, &y0, &y1);
        for (uint16_t cx = 0; cx < cols; cx++) {
            uint16_t x0, x1;
            cell_span(cx, cols, image->width, &x0, &x1);
            float rgb[FILM_CHANNELS];
            box_mean(image, x0, x1, y0, y1, rgb);
            if (ctx->bgr) {
                float b = rgb[0];
                rgb[0] = rgb[2];
                rgb[2] = b;
            }
            if (ctx->exposure) {
                for (int c = 0; c < FILM_CHANNELS; c++) {
                    rgb[c] = ctx->filter->exposure_lut[(int)rgb[c]];
                }
            }
            const uint8_t *color = s_palette[quantize_cell(rgb, cx, cy)];
            /* 格子按图像的内存通道顺序存放，放大时直接拷贝 */
            uint8_t *cell = cells + ((size_t)cy * cols + cx) * FILM_CHANNELS;
            cell[ctx->bgr ? 2 : 0] = color[0];
            cell[1] = color[1];
            cell[ctx->bgr ? 0 : 2] = color[2];
        }
    }
}

void film_pixel_upsample_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1)
{
    const film_image_t *image = ctx->image;
    const uint16_t w = image->width;
    const uint16_t h = image->height;
    const uint8_t *restrict col_cell = ctx->filter->pixel_col_cell;

    for (uint16_t y = y0; y < y1; y++) {
        uint16_t cy = (uint16_t)(((uint32_t)y * 2 + 1) * ctx->pixel_rows / (2u * h));
        const uint8_t *restrict cell_row = ctx->filter->pixel_cells + (size_t)cy * ctx->pixel_cols * FILM_CHANNELS;
        uint8_t *restrict px = image->pixels + (size_t)y * image->stride;
        for (uint16_t x = 0; x < w; x++, px += FILM_CHANNELS) {
            const uint8_t *cell = cell_row + (size_t)col_cell[x] * FILM_CHANNELS;
            px[0] = cell[0];
            px[1] = cell[1];
            px[2] = cell[2];
        }
    }
}

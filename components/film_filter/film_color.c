/*
 * 彩色/黑白管线的逐像素热循环，全部用整数和查找表：
 *   调色：曲线 → 饱和度 → 分离色调（结果先取整到 8 位，与原型的中间 uint8 一致）
 *   收尾：高光红晕 → 暗角 → 漏光 → 颗粒（中间值保持 Q8 精度，最后截断成 8 位）
 *
 * 没有高光红晕的胶卷调色和收尾在同一遍里完成（融合），像素只读写一次；
 * 有红晕的胶卷需要先得到全图遮罩再模糊，只能拆成两遍——取景时可以改用上一帧的遮罩，
 * 也合成一遍（film_color_reuse_rows）。
 *
 * 为了去掉内层循环里的分支，行函数写成 always_inline 的模板，开关（含通道顺序）作为
 * 编译期常量传入，再由分发函数为每种组合实例化一份专用循环。所有计算按逻辑 R、G、B 进行，
 * 只有读写像素时按内存顺序取下标。
 */
#include "film_filter_private.h"

#define FILM_INLINE static inline __attribute__((always_inline))

/* BT.601 亮度权重（Q16，和为 65536），对应原型的 0.299 / 0.587 / 0.114 */
#define LUMA_R_Q16  19595u
#define LUMA_G_Q16  38470u
#define LUMA_B_Q16  7471u
/* 黑白胶卷的混色权重 0.35 / 0.5 / 0.15 */
#define MONO_R_Q16  22938u
#define MONO_G_Q16  32768u
#define MONO_B_Q16  9830u

/* 高光红晕：亮度超过 THRESHOLD 开始发光，再经过 SPAN 达到满值 */
#define HALATION_THRESHOLD  190
#define HALATION_SPAN       65

/* 漏光光斑的颜色（屏幕混合） */
#define LEAK_COLOR_R        255u
#define LEAK_COLOR_G        110u
#define LEAK_COLOR_B        40u

/* 逻辑红、蓝通道在内存中的下标 */
#define IDX_R(bgr)  ((bgr) ? 2 : 0)
#define IDX_B(bgr)  ((bgr) ? 0 : 2)

static inline uint8_t clamp_u8(int32_t v)
{
    return (uint8_t)film_clamp_i32(v, 0, 255);
}

static inline uint32_t luma_q16(uint32_t r, uint32_t g, uint32_t b)
{
    return LUMA_R_Q16 * r + LUMA_G_Q16 * g + LUMA_B_Q16 * b;
}

/* ---------- 调色 ---------- */

/* 黑白：混色后查曲线；原型用截断取整作为曲线下标 */
FILM_INLINE int32_t tone_mono(const film_filter_t *f, const uint8_t *px, const bool bgr)
{
    uint32_t lum = (MONO_R_Q16 * px[IDX_R(bgr)] + MONO_G_Q16 * px[1] + MONO_B_Q16 * px[IDX_B(bgr)]) >> 16;
    return f->curve_lut[0][lum > 255 ? 255 : lum];
}

/* 彩色：曲线 → 以亮度为轴调饱和度 → 按亮度叠加暗部/亮部色调；out 为逻辑 R、G、B */
FILM_INLINE void tone_color(const film_filter_t *f, int32_t sat_q8, const uint8_t *px, int32_t out[FILM_CHANNELS],
                            const bool bgr)
{
    int32_t r = f->curve_lut[0][px[IDX_R(bgr)]];
    int32_t g = f->curve_lut[1][px[1]];
    int32_t b = f->curve_lut[2][px[IDX_B(bgr)]];
    uint32_t y16 = luma_q16((uint32_t)r, (uint32_t)g, (uint32_t)b);
    int32_t y_q8 = (int32_t)((y16 + 128) >> 8);
    uint32_t y_idx = (y16 + 32768) >> 16;
    /* 除以 Q8_ONE 而不是右移：负数要向零取整，与原型保持一致 */
    out[0] = clamp_u8((y_q8 + (((r << 8) - y_q8) * sat_q8) / Q8_ONE + f->tint_q8[0][y_idx] + Q8_ONE / 2) >> 8);
    out[1] = clamp_u8((y_q8 + (((g << 8) - y_q8) * sat_q8) / Q8_ONE + f->tint_q8[1][y_idx] + Q8_ONE / 2) >> 8);
    out[2] = clamp_u8((y_q8 + (((b << 8) - y_q8) * sat_q8) / Q8_ONE + f->tint_q8[2][y_idx] + Q8_ONE / 2) >> 8);
}

/* 调色结果（逻辑 RGB，8 位）的高光遮罩贡献 0..255 */
static inline uint32_t halation_contribution(const int32_t t[FILM_CHANNELS])
{
    int32_t lum_q8 = (int32_t)((luma_q16((uint32_t)t[0], (uint32_t)t[1], (uint32_t)t[2]) + 128) >> 8);
    int32_t hi = (lum_q8 - HALATION_THRESHOLD * Q8_ONE) * 255 / (HALATION_SPAN * Q8_ONE);
    return (uint32_t)film_clamp_i32(hi, 0, 255);
}

/* ---------- 收尾 ---------- */

/** 收尾时每行只算一次的量 */
typedef struct {
    uint32_t vignette_row;          /*!< 已加上取整偏置 */
    uint32_t leak_row;
    const int16_t *noise;           /*!< 本行的颗粒噪声，按列直接索引 */
    const uint32_t *glow_row0;      /*!< 红晕：上下两行网格和纵向权重 */
    const uint32_t *glow_row1;
    uint32_t glow_wy;
} finish_row_t;

static finish_row_t finish_row_for(const film_develop_ctx_t *ctx, uint16_t y, bool halation)
{
    const film_filter_t *f = ctx->filter;
    finish_row_t row = {
        .vignette_row = f->vignette_row[y] + (1u << (FILM_VIGNETTE_INDEX_SHIFT - 1)),
        .leak_row = f->leak_row[y],
    };
    /* 每行从噪声表的随机位置开始连续取值，行与行之间互不相关 */
    uint32_t offset = film_hash32(y * 0x85EBCA77u ^ ctx->seed * 0xC2B2AE3Du) >> (32 - FILM_GRAIN_TABLE_BITS);
    row.noise = f->grain_noise + offset;
    if (halation) {
        uint16_t gy0;
        uint8_t wy;
        film_halation_upsample_coord(y, &ctx->grid, ctx->grid.height, &gy0, &wy);
        row.glow_wy = wy;
        row.glow_row0 = f->halation_mask + (size_t)gy0 * ctx->grid.width;
        row.glow_row1 = row.glow_row0 + (wy ? ctx->grid.width : 0);
    }
    return row;
}

/* 在两行网格之间做双线性插值；列下标与权重来自预先建好的列表 */
FILM_INLINE uint32_t glow_sample(const film_filter_t *f, const finish_row_t *row, uint16_t x)
{
    uint16_t x0 = f->halation_x0[x];
    uint32_t wx = f->halation_wx[x];
    uint16_t x1 = x0 + (wx ? 1 : 0);
    uint32_t top = row->glow_row0[x0] * (Q8_ONE - wx) + row->glow_row0[x1] * wx;
    uint32_t bottom = row->glow_row1[x0] * (Q8_ONE - wx) + row->glow_row1[x1] * wx;
    return (top * (Q8_ONE - row->glow_wy) + bottom * row->glow_wy) >> 16;
}

/* 屏幕混合：out = c + (255 - c) × L / 255，全部在 Q8 下进行 */
FILM_INLINE int32_t screen_q8(int32_t v, uint32_t a_q12, uint32_t color)
{
    uint32_t leak_q8 = (a_q12 * color) >> 4;
    return v + (int32_t)(((uint32_t)(Q8_MAX - v) * leak_q8) / Q8_MAX);
}

/*
 * 对一个像素做收尾，v 为 Q8 逻辑通道值（channels == 1 表示三通道相等、只算一份）。
 * 暗角总是应用：强度为 0 的胶卷其暗角表恒为 1.0，结果不变。
 */
FILM_INLINE void finish_pixel(const film_develop_ctx_t *ctx, const finish_row_t *row, uint16_t x,
                              int32_t v[FILM_CHANNELS], const int channels,
                              const bool halation, const bool leak, const bool grain)
{
    const film_filter_t *f = ctx->filter;

    if (halation) {
        uint32_t glow = glow_sample(f, row, x);
        for (int c = 0; c < channels; c++) {
            v[c] = film_clamp_i32(v[c] + (int32_t)((glow * (uint32_t)ctx->halation_k_q16[c]) >> 8), 0, Q8_MAX);
        }
    }

    uint32_t vig_idx = (f->vignette_col[x] + row->vignette_row) >> FILM_VIGNETTE_INDEX_SHIFT;
    uint32_t mask = f->vignette_q15[vig_idx < FILM_RADIAL_LUT_SIZE ? vig_idx : FILM_RADIAL_LUT_SIZE - 1];
    for (int c = 0; c < channels; c++) {
        v[c] = (int32_t)(((uint32_t)v[c] * mask) >> 15);
    }

    if (leak) {
        uint32_t leak_idx = (f->leak_col[x] + row->leak_row) >> FILM_LEAK_INDEX_SHIFT;
        uint32_t a_q12 = f->leak_q12[leak_idx < FILM_RADIAL_LUT_SIZE ? leak_idx : FILM_RADIAL_LUT_SIZE - 1];
        if (a_q12) {
            v[0] = screen_q8(v[0], a_q12, LEAK_COLOR_R);
            v[1] = screen_q8(v[1], a_q12, LEAK_COLOR_G);
            v[2] = screen_q8(v[2], a_q12, LEAK_COLOR_B);
        }
    }

    if (grain) {
        /* 三通道相等时亮度就是该通道值（亮度权重和为 1） */
        uint32_t y_idx = channels == 1
                         ? (uint32_t)v[0] >> 8
                         : luma_q16((uint32_t)v[0], (uint32_t)v[1], (uint32_t)v[2]) >> 24;
        int32_t add_q8 = (row->noise[x] * (int32_t)f->grain_amp_q12[y_idx]) / 16;
        for (int c = 0; c < channels; c++) {
            v[c] += add_q8;
        }
    }
}

/* 原型最后用 astype(uint8) 截断取整；v 为逻辑 RGB */
FILM_INLINE void store_pixel(uint8_t *px, const int32_t v[FILM_CHANNELS], const int channels, const bool bgr)
{
    if (channels == 1) {
        uint8_t out = (uint8_t)(film_clamp_i32(v[0], 0, Q8_MAX) >> 8);
        px[0] = px[1] = px[2] = out;
    } else {
        px[IDX_R(bgr)] = (uint8_t)(film_clamp_i32(v[0], 0, Q8_MAX) >> 8);
        px[1] = (uint8_t)(film_clamp_i32(v[1], 0, Q8_MAX) >> 8);
        px[IDX_B(bgr)] = (uint8_t)(film_clamp_i32(v[2], 0, Q8_MAX) >> 8);
    }
}

/* 调色一个像素，结果为逻辑 RGB 的 8 位值 */
FILM_INLINE void tone_pixel(const film_develop_ctx_t *ctx, const uint8_t *px, int32_t t[FILM_CHANNELS],
                            const bool mono, const bool bgr)
{
    if (mono) {
        t[0] = t[1] = t[2] = tone_mono(ctx->filter, px, bgr);
    } else {
        tone_color(ctx->filter, ctx->sat_q8, px, t, bgr);
    }
}

/* ---------- 融合单遍：调色 + 收尾（无红晕） ---------- */

FILM_INLINE void fused_rows_impl(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1,
                                 const bool mono, const bool leak, const bool grain, const bool bgr)
{
    const film_image_t *image = ctx->image;
    const uint16_t width = image->width;
    /* 漏光有颜色，黑白胶卷叠了漏光后三通道就不再相等 */
    const int channels = (mono && !leak) ? 1 : FILM_CHANNELS;

    for (uint16_t y = y0; y < y1; y++) {
        uint8_t *restrict px = image->pixels + (size_t)y * image->stride;
        const finish_row_t row = finish_row_for(ctx, y, false);
        for (uint16_t x = 0; x < width; x++, px += FILM_CHANNELS) {
            int32_t v[FILM_CHANNELS];
            tone_pixel(ctx, px, v, mono, bgr);
            v[0] <<= 8;
            v[1] <<= 8;
            v[2] <<= 8;
            finish_pixel(ctx, &row, x, v, channels, false, leak, grain);
            store_pixel(px, v, channels, bgr);
        }
    }
}

/* 为开关的每种组合实例化一份循环：key 的位依次为 bgr(8) mono(4) leak(2) grain(1) */
#define FUSED_CASE(k) \
    case k: fused_rows_impl(ctx, y0, y1, (k) & 4, (k) & 2, (k) & 1, (k) & 8); break

void film_color_fused_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1)
{
    switch ((ctx->bgr ? 8 : 0) | (ctx->mono ? 4 : 0) | (ctx->leak ? 2 : 0) | (ctx->grain ? 1 : 0)) {
        FUSED_CASE(0); FUSED_CASE(1); FUSED_CASE(2); FUSED_CASE(3);
        FUSED_CASE(4); FUSED_CASE(5); FUSED_CASE(6); FUSED_CASE(7);
        FUSED_CASE(8); FUSED_CASE(9); FUSED_CASE(10); FUSED_CASE(11);
        FUSED_CASE(12); FUSED_CASE(13); FUSED_CASE(14);
    default: fused_rows_impl(ctx, y0, y1, true, true, true, true); break;
    }
}

/* ---------- 红晕第一遍：调色 + 累计高光遮罩 ---------- */

FILM_INLINE void tone_rows_impl(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1,
                                const bool mono, const bool bgr)
{
    const film_image_t *image = ctx->image;
    const uint16_t width = image->width;
    const uint16_t factor = ctx->grid.factor;

    for (uint16_t y = y0; y < y1; y++) {
        uint8_t *restrict px = image->pixels + (size_t)y * image->stride;
        uint32_t *restrict mask_row = ctx->halation_accum + (size_t)(y / factor) * ctx->grid.width;
        uint16_t cell_x = 0;
        uint16_t cell_left = factor;

        for (uint16_t x = 0; x < width; x++, px += FILM_CHANNELS) {
            int32_t t[FILM_CHANNELS];
            tone_pixel(ctx, px, t, mono, bgr);
            px[IDX_R(bgr)] = (uint8_t)t[0];
            px[1] = (uint8_t)t[1];
            px[IDX_B(bgr)] = (uint8_t)t[2];
            mask_row[cell_x] += halation_contribution(t);
            if (--cell_left == 0) {
                cell_left = factor;
                cell_x++;
            }
        }
    }
}

void film_color_tone_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1)
{
    switch ((ctx->bgr ? 2 : 0) | (ctx->mono ? 1 : 0)) {
    case 0: tone_rows_impl(ctx, y0, y1, false, false); break;
    case 1: tone_rows_impl(ctx, y0, y1, true, false); break;
    case 2: tone_rows_impl(ctx, y0, y1, false, true); break;
    default: tone_rows_impl(ctx, y0, y1, true, true); break;
    }
}

/* ---------- 红晕第二遍：红晕 + 收尾 ---------- */

FILM_INLINE void finish_rows_impl(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1,
                                  const bool leak, const bool grain, const bool bgr)
{
    const film_image_t *image = ctx->image;
    const uint16_t width = image->width;

    for (uint16_t y = y0; y < y1; y++) {
        uint8_t *restrict px = image->pixels + (size_t)y * image->stride;
        const finish_row_t row = finish_row_for(ctx, y, true);
        for (uint16_t x = 0; x < width; x++, px += FILM_CHANNELS) {
            int32_t v[FILM_CHANNELS] = { px[IDX_R(bgr)] << 8, px[1] << 8, px[IDX_B(bgr)] << 8 };
            finish_pixel(ctx, &row, x, v, FILM_CHANNELS, true, leak, grain);
            store_pixel(px, v, FILM_CHANNELS, bgr);
        }
    }
}

#define FINISH_CASE(k) \
    case k: finish_rows_impl(ctx, y0, y1, (k) & 2, (k) & 1, (k) & 4); break

void film_color_finish_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1)
{
    switch ((ctx->bgr ? 4 : 0) | (ctx->leak ? 2 : 0) | (ctx->grain ? 1 : 0)) {
        FINISH_CASE(0); FINISH_CASE(1); FINISH_CASE(2); FINISH_CASE(3);
        FINISH_CASE(4); FINISH_CASE(5); FINISH_CASE(6);
    default: finish_rows_impl(ctx, y0, y1, true, true, true); break;
    }
}

/* ---------- 快速红晕：上一帧遮罩收尾 + 累计本帧遮罩，一遍完成 ---------- */

FILM_INLINE void reuse_rows_impl(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1,
                                 const bool mono, const bool leak, const bool grain, const bool bgr)
{
    const film_image_t *image = ctx->image;
    const uint16_t width = image->width;
    const uint16_t factor = ctx->grid.factor;

    for (uint16_t y = y0; y < y1; y++) {
        uint8_t *restrict px = image->pixels + (size_t)y * image->stride;
        uint32_t *restrict mask_row = ctx->halation_accum + (size_t)(y / factor) * ctx->grid.width;
        const finish_row_t row = finish_row_for(ctx, y, true);
        uint16_t cell_x = 0;
        uint16_t cell_left = factor;

        for (uint16_t x = 0; x < width; x++, px += FILM_CHANNELS) {
            int32_t v[FILM_CHANNELS];
            tone_pixel(ctx, px, v, mono, bgr);
            mask_row[cell_x] += halation_contribution(v);
            if (--cell_left == 0) {
                cell_left = factor;
                cell_x++;
            }
            v[0] <<= 8;
            v[1] <<= 8;
            v[2] <<= 8;
            finish_pixel(ctx, &row, x, v, FILM_CHANNELS, true, leak, grain);
            store_pixel(px, v, FILM_CHANNELS, bgr);
        }
    }
}

#define REUSE_CASE(k) \
    case k: reuse_rows_impl(ctx, y0, y1, (k) & 4, (k) & 2, (k) & 1, (k) & 8); break

void film_color_reuse_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1)
{
    switch ((ctx->bgr ? 8 : 0) | (ctx->mono ? 4 : 0) | (ctx->leak ? 2 : 0) | (ctx->grain ? 1 : 0)) {
        REUSE_CASE(0); REUSE_CASE(1); REUSE_CASE(2); REUSE_CASE(3);
        REUSE_CASE(4); REUSE_CASE(5); REUSE_CASE(6); REUSE_CASE(7);
        REUSE_CASE(8); REUSE_CASE(9); REUSE_CASE(10); REUSE_CASE(11);
        REUSE_CASE(12); REUSE_CASE(13); REUSE_CASE(14);
    default: reuse_rows_impl(ctx, y0, y1, true, true, true, true); break;
    }
}

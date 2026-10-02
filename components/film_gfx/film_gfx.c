/*
 * film_gfx 软件光栅实现。
 *
 * 所有绘制都按"先求裁剪后的像素范围，再逐像素混合"的方式进行；抗锯齿只在边缘（圆角、圆、三角形）
 * 逐像素计算覆盖率，内部像素走快速路径。混合在 RGB565 的 5/6/5 位分量上用 8 位透明度完成。
 */
#include "film_gfx.h"

#include <math.h>
#include <string.h>

#define Q4_ONE 16

static inline int imin(int a, int b)
{
    return a < b ? a : b;
}

static inline int imax(int a, int b)
{
    return a > b ? a : b;
}

static inline float fclamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/** 近似 v / 255（v ≤ 255·255），四舍五入 */
static inline uint32_t div255(uint32_t v)
{
    v += 128;
    return (v + (v >> 8)) >> 8;
}

/** 把 s 以透明度 a（0..255）叠到 d 上 */
static inline uint16_t blend565(uint16_t d, uint16_t s, uint32_t a)
{
    if (a == 0) {
        return d;
    }
    if (a >= 255) {
        return s;
    }
    const uint32_t ia = 255 - a;
    const uint32_t r = div255((uint32_t)(d >> 11) * ia + (uint32_t)(s >> 11) * a);
    const uint32_t g = div255((uint32_t)((d >> 5) & 63) * ia + (uint32_t)((s >> 5) & 63) * a);
    const uint32_t b = div255((uint32_t)(d & 31) * ia + (uint32_t)(s & 31) * a);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t *pixel_at(const gfx_canvas_t *c, int x, int y)
{
    return c->pixels + (int32_t)y * c->stride + x;
}

/** 求 r 与裁剪区的交集，空则返回 false；输出为半开区间 [x0, x1) × [y0, y1) */
static bool clip_span(const gfx_canvas_t *c, gfx_rect_t r, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = imax(r.x, c->clip.x);
    *y0 = imax(r.y, c->clip.y);
    *x1 = imin(r.x + r.w, c->clip.x + c->clip.w);
    *y1 = imin(r.y + r.h, c->clip.y + c->clip.h);
    return *x0 < *x1 && *y0 < *y1;
}

/* ---------------------------------------------------------------- 画布 */

void gfx_canvas_init(gfx_canvas_t *canvas, uint16_t *pixels, int width, int height, int stride)
{
    canvas->pixels = pixels;
    canvas->width = (int16_t)width;
    canvas->height = (int16_t)height;
    canvas->stride = stride;
    canvas->clip = gfx_rect(0, 0, width, height);
}

gfx_rect_t gfx_clip_push(gfx_canvas_t *canvas, gfx_rect_t rect)
{
    const gfx_rect_t previous = canvas->clip;
    int x0, y0, x1, y1;
    if (clip_span(canvas, rect, &x0, &y0, &x1, &y1)) {
        canvas->clip = gfx_rect(x0, y0, x1 - x0, y1 - y0);
    } else {
        canvas->clip = gfx_rect(0, 0, 0, 0);
    }
    return previous;
}

void gfx_clip_pop(gfx_canvas_t *canvas, gfx_rect_t previous)
{
    canvas->clip = previous;
}

/* ---------------------------------------------------------------- 圆角覆盖率 */

/** 像素 (px, py) 落在圆角矩形内的覆盖率 0..255；矩形外为 0 */
static uint32_t round_cov(gfx_rect_t r, int radius, int px, int py)
{
    if (px < r.x || py < r.y || px >= r.x + r.w || py >= r.y + r.h) {
        return 0;
    }
    if (radius <= 0) {
        return 255;
    }
    const bool in_x_band = px < r.x + radius || px >= r.x + r.w - radius;
    const bool in_y_band = py < r.y + radius || py >= r.y + r.h - radius;
    if (!in_x_band || !in_y_band) {
        return 255;
    }
    const float fx = (float)px + 0.5f;
    const float fy = (float)py + 0.5f;
    const float cx = fx < (float)(r.x + radius) ? (float)(r.x + radius) : (float)(r.x + r.w - radius);
    const float cy = fy < (float)(r.y + radius) ? (float)(r.y + radius) : (float)(r.y + r.h - radius);
    const float dx = fx - cx;
    const float dy = fy - cy;
    const float cov = fclamp01((float)radius - sqrtf(dx * dx + dy * dy) + 0.5f);
    return (uint32_t)(cov * 255.0f + 0.5f);
}

/* ---------------------------------------------------------------- 填充 */

void gfx_fill(gfx_canvas_t *c, gfx_rect_t r, uint32_t color, uint8_t alpha)
{
    int x0, y0, x1, y1;
    if (alpha == 0 || !clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint16_t s = gfx_rgb565(color);
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        if (alpha == 255) {
            for (int x = x0; x < x1; ++x) {
                row[x] = s;
            }
        } else {
            for (int x = x0; x < x1; ++x) {
                row[x] = blend565(row[x], s, alpha);
            }
        }
    }
}

void gfx_fill_round(gfx_canvas_t *c, gfx_rect_t r, int radius, uint32_t color, uint8_t alpha)
{
    int x0, y0, x1, y1;
    if (alpha == 0 || !clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint16_t s = gfx_rgb565(color);
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        for (int x = x0; x < x1; ++x) {
            row[x] = blend565(row[x], s, div255(round_cov(r, radius, x, y) * alpha));
        }
    }
}

void gfx_stroke_round(gfx_canvas_t *c, gfx_rect_t r, int radius, int width, uint32_t color, uint8_t alpha)
{
    int x0, y0, x1, y1;
    if (alpha == 0 || width <= 0 || !clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const gfx_rect_t inner = gfx_rect(r.x + width, r.y + width, r.w - 2 * width, r.h - 2 * width);
    const int inner_radius = imax(radius - width, 0);
    const bool has_inner = inner.w > 0 && inner.h > 0;
    const uint16_t s = gfx_rgb565(color);
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        /* 内外两层覆盖率都是 255 的中段整段跳过（大框只需扫两侧边与四角，结果逐像素不变） */
        int skip0 = x1;
        int skip1 = x1;
        if (has_inner && y >= inner.y && y < inner.y + inner.h) {
            const bool corner_row = y < r.y + radius || y >= r.y + r.h - radius ||
                                    y < inner.y + inner_radius || y >= inner.y + inner.h - inner_radius;
            skip0 = corner_row ? imax(r.x + radius, inner.x + inner_radius) : inner.x;
            skip1 = corner_row ? imin(r.x + r.w - radius, inner.x + inner.w - inner_radius) : inner.x + inner.w;
        }
        for (int x = x0; x < x1; ++x) {
            if (x >= skip0 && x < skip1) {
                x = skip1 - 1;
                continue;
            }
            const int cov = (int)round_cov(r, radius, x, y) -
                            (has_inner ? (int)round_cov(inner, inner_radius, x, y) : 0);
            if (cov > 0) {
                row[x] = blend565(row[x], s, div255((uint32_t)cov * alpha));
            }
        }
    }
}

static inline uint32_t lerp_rgb(uint32_t a, uint32_t b, int t_q8)
{
    uint32_t out = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        const int ca = (int)((a >> shift) & 0xFF);
        const int cb = (int)((b >> shift) & 0xFF);
        out |= (uint32_t)(ca + (((cb - ca) * t_q8) >> 8)) << shift;
    }
    return out;
}

void gfx_gradient_v(gfx_canvas_t *c, gfx_rect_t r, int radius, uint32_t top, uint8_t top_alpha,
                    uint32_t bottom, uint8_t bottom_alpha)
{
    int x0, y0, x1, y1;
    if (!clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const int span = imax(r.h - 1, 1);
    for (int y = y0; y < y1; ++y) {
        const int t = ((y - r.y) * 256) / span;
        const uint16_t s = gfx_rgb565(lerp_rgb(top, bottom, t));
        const uint32_t a = (uint32_t)(top_alpha + (((int)bottom_alpha - (int)top_alpha) * t >> 8));
        if (a == 0) {
            continue;
        }
        uint16_t *row = pixel_at(c, 0, y);
        for (int x = x0; x < x1; ++x) {
            row[x] = blend565(row[x], s, div255(round_cov(r, radius, x, y) * a));
        }
    }
}

void gfx_gradient_h(gfx_canvas_t *c, gfx_rect_t r, uint32_t left, uint8_t left_alpha,
                    uint32_t right, uint8_t right_alpha)
{
    int x0, y0, x1, y1;
    if (!clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const int span = imax(r.w - 1, 1);
    for (int x = x0; x < x1; ++x) {
        const int t = ((x - r.x) * 256) / span;
        const uint16_t s = gfx_rgb565(lerp_rgb(left, right, t));
        const uint32_t a = (uint32_t)(left_alpha + (((int)right_alpha - (int)left_alpha) * t >> 8));
        if (a == 0) {
            continue;
        }
        for (int y = y0; y < y1; ++y) {
            uint16_t *p = pixel_at(c, x, y);
            *p = blend565(*p, s, a);
        }
    }
}

/** 圆形覆盖率的通用实现：半径 rad（像素，浮点），返回 0..1 */
static inline float disc_cov(float dx, float dy, float rad)
{
    return fclamp01(rad - sqrtf(dx * dx + dy * dy) + 0.5f);
}

void gfx_circle_q4(gfx_canvas_t *c, int cx_q4, int cy_q4, int radius_q4, uint32_t color, uint8_t alpha)
{
    gfx_ring_q4(c, cx_q4, cy_q4, radius_q4, radius_q4 + Q4_ONE, color, alpha);
}

void gfx_ring_q4(gfx_canvas_t *c, int cx_q4, int cy_q4, int radius_q4, int width_q4, uint32_t color, uint8_t alpha)
{
    const int rad_px = (radius_q4 + Q4_ONE - 1) / Q4_ONE + 1;
    const gfx_rect_t box = gfx_rect(cx_q4 / Q4_ONE - rad_px, cy_q4 / Q4_ONE - rad_px, 2 * rad_px + 1, 2 * rad_px + 1);
    int x0, y0, x1, y1;
    if (alpha == 0 || !clip_span(c, box, &x0, &y0, &x1, &y1)) {
        return;
    }
    const float cx = (float)cx_q4 / Q4_ONE;
    const float cy = (float)cy_q4 / Q4_ONE;
    const float outer = (float)radius_q4 / Q4_ONE;
    const float inner = (float)(radius_q4 - width_q4) / Q4_ONE;
    const uint16_t s = gfx_rgb565(color);
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        const float dy = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; ++x) {
            const float dx = (float)x + 0.5f - cx;
            float cov = disc_cov(dx, dy, outer);
            if (inner > 0.0f) {
                cov -= disc_cov(dx, dy, inner);
            }
            if (cov > 0.0f) {
                row[x] = blend565(row[x], s, (uint32_t)(cov * (float)alpha + 0.5f));
            }
        }
    }
}

void gfx_glow(gfx_canvas_t *c, int cx, int cy, int radius, uint32_t color, uint8_t alpha)
{
    int x0, y0, x1, y1;
    if (alpha == 0 || radius <= 0 ||
        !clip_span(c, gfx_rect(cx - radius, cy - radius, 2 * radius + 1, 2 * radius + 1), &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint16_t s = gfx_rgb565(color);
    const float inv = 1.0f / (float)radius;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        const float dy = (float)(y - cy);
        for (int x = x0; x < x1; ++x) {
            const float dx = (float)(x - cx);
            const float t = 1.0f - sqrtf(dx * dx + dy * dy) * inv;
            if (t > 0.0f) {
                row[x] = blend565(row[x], s, (uint32_t)(t * t * (float)alpha));
            }
        }
    }
}

void gfx_triangle_q4(gfx_canvas_t *c, int ax, int ay, int bx, int by, int cx, int cy, uint32_t color, uint8_t alpha)
{
    const int minx = imin(ax, imin(bx, cx)) / Q4_ONE - 1;
    const int miny = imin(ay, imin(by, cy)) / Q4_ONE - 1;
    const int maxx = imax(ax, imax(bx, cx)) / Q4_ONE + 1;
    const int maxy = imax(ay, imax(by, cy)) / Q4_ONE + 1;
    int x0, y0, x1, y1;
    if (alpha == 0 || !clip_span(c, gfx_rect(minx, miny, maxx - minx + 1, maxy - miny + 1), &x0, &y0, &x1, &y1)) {
        return;
    }
    /* 让三个顶点按同一方向排列，三条边函数都 ≥0 即在内部 */
    const int64_t area = (int64_t)(bx - ax) * (cy - ay) - (int64_t)(by - ay) * (cx - ax);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        int t = bx;
        bx = cx;
        cx = t;
        t = by;
        by = cy;
        cy = t;
    }
    const uint16_t s = gfx_rgb565(color);
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        for (int x = x0; x < x1; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) {
                const int py = y * Q4_ONE + 2 + sy * 4;
                for (int sx = 0; sx < 4; ++sx) {
                    const int px = x * Q4_ONE + 2 + sx * 4;
                    const int64_t e0 = (int64_t)(bx - ax) * (py - ay) - (int64_t)(by - ay) * (px - ax);
                    const int64_t e1 = (int64_t)(cx - bx) * (py - by) - (int64_t)(cy - by) * (px - bx);
                    const int64_t e2 = (int64_t)(ax - cx) * (py - cy) - (int64_t)(ay - cy) * (px - cx);
                    hits += (e0 >= 0 && e1 >= 0 && e2 >= 0);
                }
            }
            if (hits) {
                row[x] = blend565(row[x], s, (uint32_t)(hits * alpha) / 16);
            }
        }
    }
}

#define QUAD_SUBLINES 4

/** 凸四边形与水平线 y 的交段 [*xl, *xr]；不相交返回 false */
static bool quad_span(const float *px, const float *py, float y, float *xl, float *xr)
{
    float lo = 1e9f, hi = -1e9f;
    for (int i = 0; i < 4; ++i) {
        const int j = (i + 1) & 3;
        const float ya = py[i], yb = py[j];
        if ((y < ya && y < yb) || (y >= ya && y >= yb)) {
            continue;   /* 半开区间：顶点只算一次，水平边不算 */
        }
        const float x = px[i] + (px[j] - px[i]) * (y - ya) / (yb - ya);
        lo = fminf(lo, x);
        hi = fmaxf(hi, x);
    }
    *xl = lo;
    *xr = hi;
    return lo < hi;
}

void gfx_quad_q4(gfx_canvas_t *c, const int pts_q4[8], uint32_t color, uint8_t alpha)
{
    float px[4], py[4];
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < 4; ++i) {
        px[i] = (float)pts_q4[2 * i] / Q4_ONE;
        py[i] = (float)pts_q4[2 * i + 1] / Q4_ONE;
        minx = fminf(minx, px[i]);
        maxx = fmaxf(maxx, px[i]);
        miny = fminf(miny, py[i]);
        maxy = fmaxf(maxy, py[i]);
    }
    const gfx_rect_t box = gfx_rect((int)floorf(minx), (int)floorf(miny), (int)ceilf(maxx) - (int)floorf(minx) + 1,
                                    (int)ceilf(maxy) - (int)floorf(miny) + 1);
    int x0, y0, x1, y1;
    if (alpha == 0 || !clip_span(c, box, &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint16_t s = gfx_rgb565(color);
    for (int y = y0; y < y1; ++y) {
        /* 本行 4 条子扫描线各自的交段；全覆盖区间取它们的交集 */
        float xl[QUAD_SUBLINES], xr[QUAD_SUBLINES];
        int lines = 0;
        float span_lo = 1e9f, span_hi = -1e9f, full_lo = -1e9f, full_hi = 1e9f;
        for (int k = 0; k < QUAD_SUBLINES; ++k) {
            const float sy = (float)y + ((float)k + 0.5f) / QUAD_SUBLINES;
            if (!quad_span(px, py, sy, &xl[lines], &xr[lines])) {
                full_lo = 1e9f;   /* 有子线不相交：这一行没有全覆盖的像素 */
                continue;
            }
            span_lo = fminf(span_lo, xl[lines]);
            span_hi = fmaxf(span_hi, xr[lines]);
            full_lo = fmaxf(full_lo, xl[lines]);
            full_hi = fminf(full_hi, xr[lines]);
            ++lines;
        }
        if (!lines) {
            continue;
        }
        const int a = imax(x0, (int)floorf(span_lo));
        const int b = imin(x1, (int)ceilf(span_hi));
        const int fa = imax(a, (int)ceilf(full_lo));
        const int fb = imin(b, (int)floorf(full_hi));
        uint16_t *row = pixel_at(c, 0, y);
        for (int x = a; x < b; ++x) {
            if (x >= fa && x < fb) {
                row[x] = blend565(row[x], s, alpha);
                continue;
            }
            float cov = 0.0f;
            for (int k = 0; k < lines; ++k) {
                cov += fmaxf(0.0f, fminf(xr[k], (float)x + 1.0f) - fmaxf(xl[k], (float)x));
            }
            const uint32_t al = (uint32_t)(cov * (float)alpha / QUAD_SUBLINES + 0.5f);
            if (al) {
                row[x] = blend565(row[x], s, al);
            }
        }
    }
}

void gfx_shadow(gfx_canvas_t *c, gfx_rect_t r, int radius, int blur, uint32_t color, uint8_t alpha)
{
    const gfx_rect_t outer = gfx_rect(r.x - blur, r.y - blur, r.w + 2 * blur, r.h + 2 * blur);
    int x0, y0, x1, y1;
    if (alpha == 0 || blur <= 0 || !clip_span(c, outer, &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint16_t s = gfx_rgb565(color);
    const float left = (float)(r.x + radius);
    const float right = (float)(r.x + r.w - radius);
    const float top = (float)(r.y + radius);
    const float bottom = (float)(r.y + r.h - radius);
    const float inv_blur = 1.0f / (float)blur;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        const float fy = (float)y + 0.5f;
        const float dy = fy < top ? top - fy : (fy > bottom ? fy - bottom : 0.0f);
        for (int x = x0; x < x1; ++x) {
            const float fx = (float)x + 0.5f;
            const float dx = fx < left ? left - fx : (fx > right ? fx - right : 0.0f);
            const float d = sqrtf(dx * dx + dy * dy) - (float)radius;
            const float t = d <= 0.0f ? 1.0f : 1.0f - d * inv_blur;
            if (t > 0.0f) {
                row[x] = blend565(row[x], s, (uint32_t)(t * t * (float)alpha));
            }
        }
    }
}

void gfx_dim(gfx_canvas_t *c, gfx_rect_t r, int factor_q8)
{
    int x0, y0, x1, y1;
    if (factor_q8 >= 256 || !clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint32_t f = (uint32_t)imax(factor_q8, 0);
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        for (int x = x0; x < x1; ++x) {
            const uint16_t p = row[x];
            const uint32_t rr = ((uint32_t)(p >> 11) * f) >> 8;
            const uint32_t gg = ((uint32_t)((p >> 5) & 63) * f) >> 8;
            const uint32_t bb = ((uint32_t)(p & 31) * f) >> 8;
            row[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
        }
    }
}

/* ---------------------------------------------------------------- 图片 */

void gfx_blit_part(gfx_canvas_t *c, const gfx_image_t *img, gfx_rect_t src, int x, int y, uint8_t opacity)
{
    int x0, y0, x1, y1;
    if (!img->pixels || opacity == 0 || !clip_span(c, gfx_rect(x, y, src.w, src.h), &x0, &y0, &x1, &y1)) {
        return;
    }
    for (int dy = y0; dy < y1; ++dy) {
        const int sy = src.y + dy - y;
        const uint16_t *s = img->pixels + (int32_t)sy * img->width + src.x - x;
        const uint8_t *a = img->alpha ? img->alpha + (int32_t)sy * img->width + src.x - x : NULL;
        uint16_t *row = pixel_at(c, 0, dy);
        if (!a && opacity == 255) {
            memcpy(row + x0, s + x0, (size_t)(x1 - x0) * sizeof(uint16_t));
            continue;
        }
        for (int dx = x0; dx < x1; ++dx) {
            const uint32_t al = a ? (opacity == 255 ? a[dx] : div255((uint32_t)a[dx] * opacity)) : opacity;
            row[dx] = blend565(row[dx], s[dx], al);
        }
    }
}

void gfx_blit(gfx_canvas_t *c, const gfx_image_t *img, int x, int y, uint8_t opacity)
{
    gfx_blit_part(c, img, gfx_rect(0, 0, img->width, img->height), x, y, opacity);
}

void gfx_copy(gfx_canvas_t *c, const uint16_t *pixels, int width, int height, int stride, int x, int y)
{
    const gfx_image_t img = { pixels, NULL, (uint16_t)stride, (uint16_t)height };
    gfx_blit_part(c, &img, gfx_rect(0, 0, width, height), x, y, 255);
}

/** 双线性取 RGB565 四邻像素（坐标为 16.16 定点，已保证不越界） */
static inline uint16_t sample_bilinear(const uint16_t *pixels, int stride, int w, int h, int32_t u, int32_t v)
{
    int x = u >> 16;
    int y = v >> 16;
    uint32_t fx = (uint32_t)(u >> 8) & 0xFF;
    uint32_t fy = (uint32_t)(v >> 8) & 0xFF;
    if (x < 0) {
        x = 0;
        fx = 0;
    }
    if (y < 0) {
        y = 0;
        fy = 0;
    }
    const int x2 = imin(x + 1, w - 1);
    const int y2 = imin(y + 1, h - 1);
    x = imin(x, w - 1);
    y = imin(y, h - 1);
    const uint16_t p00 = pixels[(int32_t)y * stride + x];
    const uint16_t p01 = pixels[(int32_t)y * stride + x2];
    const uint16_t p10 = pixels[(int32_t)y2 * stride + x];
    const uint16_t p11 = pixels[(int32_t)y2 * stride + x2];
    if (p00 == p01 && p00 == p10 && p00 == p11) {
        return p00;
    }
    /* 拆成 0x07E0F81F 的"隔位"格式，一次乘法同时插值三个分量 */
    const uint32_t a = ((uint32_t)p00 | ((uint32_t)p00 << 16)) & 0x07E0F81FU;
    const uint32_t b = ((uint32_t)p01 | ((uint32_t)p01 << 16)) & 0x07E0F81FU;
    const uint32_t cc = ((uint32_t)p10 | ((uint32_t)p10 << 16)) & 0x07E0F81FU;
    const uint32_t d = ((uint32_t)p11 | ((uint32_t)p11 << 16)) & 0x07E0F81FU;
    const uint32_t fx5 = fx >> 3;
    const uint32_t fy5 = fy >> 3;
    const uint32_t top = (a * (32 - fx5) + b * fx5) >> 5 & 0x07E0F81FU;
    const uint32_t bot = (cc * (32 - fx5) + d * fx5) >> 5 & 0x07E0F81FU;
    const uint32_t m = (top * (32 - fy5) + bot * fy5) >> 5 & 0x07E0F81FU;
    return (uint16_t)(m | (m >> 16));
}

void gfx_blit_scaled(gfx_canvas_t *c, const uint16_t *pixels, int width, int height, int stride,
                     gfx_rect_t src, gfx_rect_t dst, uint8_t opacity)
{
    int x0, y0, x1, y1;
    if (!pixels || opacity == 0 || src.w <= 0 || src.h <= 0 || !clip_span(c, dst, &x0, &y0, &x1, &y1)) {
        return;
    }
    if (src.w == dst.w && src.h == dst.h) {
        const gfx_image_t img = { pixels, NULL, (uint16_t)stride, (uint16_t)height };
        gfx_blit_part(c, &img, src, dst.x, dst.y, opacity);
        return;
    }
    const int32_t step_x = (int32_t)(((int64_t)src.w << 16) / dst.w);
    const int32_t step_y = (int32_t)(((int64_t)src.h << 16) / dst.h);
    const int32_t base_u = ((int32_t)src.x << 16) + step_x / 2 - 0x8000;
    const int32_t base_v = ((int32_t)src.y << 16) + step_y / 2 - 0x8000;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        const int32_t v = base_v + (y - dst.y) * step_y;
        for (int x = x0; x < x1; ++x) {
            const int32_t u = base_u + (x - dst.x) * step_x;
            const uint16_t s = sample_bilinear(pixels, stride, width, height, u, v);
            row[x] = opacity == 255 ? s : blend565(row[x], s, opacity);
        }
    }
}

void gfx_blit_cover(gfx_canvas_t *c, const uint16_t *pixels, int width, int height, int stride, gfx_rect_t dst,
                    uint8_t opacity)
{
    if (dst.w <= 0 || dst.h <= 0 || width <= 0 || height <= 0) {
        return;
    }
    gfx_rect_t src = gfx_rect(0, 0, width, height);
    if ((int32_t)width * dst.h > (int32_t)height * dst.w) {
        src.w = (int16_t)(((int32_t)height * dst.w) / dst.h);
        src.x = (int16_t)((width - src.w) / 2);
    } else {
        src.h = (int16_t)(((int32_t)width * dst.h) / dst.w);
        src.y = (int16_t)((height - src.h) / 2);
    }
    gfx_blit_scaled(c, pixels, width, height, stride, src, dst, opacity);
}

void gfx_blit_rotated(gfx_canvas_t *c, const uint16_t *pixels, const uint8_t *alpha, int width, int height,
                      int stride, int cx_q4, int cy_q4, float angle_deg, float scale, uint8_t opacity)
{
    if (!pixels || opacity == 0 || scale <= 0.0f) {
        return;
    }
    const float rad = angle_deg * 3.14159265f / 180.0f;
    const float cs = cosf(rad);
    const float sn = sinf(rad);
    const float hw = (float)width * 0.5f * scale;
    const float hh = (float)height * 0.5f * scale;
    const float ex = fabsf(hw * cs) + fabsf(hh * sn);
    const float ey = fabsf(hw * sn) + fabsf(hh * cs);
    const float cx = (float)cx_q4 / Q4_ONE;
    const float cy = (float)cy_q4 / Q4_ONE;
    int x0, y0, x1, y1;
    const gfx_rect_t box = gfx_rect((int)floorf(cx - ex) - 1, (int)floorf(cy - ey) - 1, (int)(2 * ex) + 3,
                                    (int)(2 * ey) + 3);
    if (!clip_span(c, box, &x0, &y0, &x1, &y1)) {
        return;
    }
    const float inv = 1.0f / scale;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        const float py = (float)y + 0.5f - cy;
        for (int x = x0; x < x1; ++x) {
            const float px = (float)x + 0.5f - cx;
            /* 逆旋转回源图坐标（像素中心为 .5） */
            const float u = (px * cs + py * sn) * inv + (float)width * 0.5f - 0.5f;
            const float v = (-px * sn + py * cs) * inv + (float)height * 0.5f - 0.5f;
            if (u < -1.0f || v < -1.0f || u >= (float)width || v >= (float)height) {
                continue;
            }
            const int ix = (int)floorf(u);
            const int iy = (int)floorf(v);
            const float fx = u - (float)ix;
            const float fy = v - (float)iy;
            float acc_a = 0.0f, acc_r = 0.0f, acc_g = 0.0f, acc_b = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const int sx = ix + (k & 1);
                const int sy = iy + (k >> 1);
                if (sx < 0 || sy < 0 || sx >= width || sy >= height) {
                    continue;
                }
                const float w = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
                const float a = (alpha ? (float)alpha[(int32_t)sy * stride + sx] : 255.0f) * w;
                const uint16_t p = pixels[(int32_t)sy * stride + sx];
                acc_a += a;
                acc_r += a * (float)(p >> 11);
                acc_g += a * (float)((p >> 5) & 63);
                acc_b += a * (float)(p & 31);
            }
            if (acc_a < 0.5f) {
                continue;
            }
            const float inv_a = 1.0f / acc_a;
            const uint16_t s = (uint16_t)(((uint32_t)(acc_r * inv_a + 0.5f) << 11) |
                                          ((uint32_t)(acc_g * inv_a + 0.5f) << 5) | (uint32_t)(acc_b * inv_a + 0.5f));
            row[x] = blend565(row[x], s, (uint32_t)(acc_a * (float)opacity / 255.0f + 0.5f));
        }
    }
}

void gfx_tile(gfx_canvas_t *c, const gfx_image_t *tex, gfx_rect_t r, int origin_x, int origin_y, int radius)
{
    int x0, y0, x1, y1;
    if (!tex->pixels || !clip_span(c, r, &x0, &y0, &x1, &y1)) {
        return;
    }
    const int tw = tex->width;
    const int th = tex->height;
    for (int y = y0; y < y1; ++y) {
        uint16_t *row = pixel_at(c, 0, y);
        const int ty = (((y - origin_y) % th) + th) % th;
        const uint16_t *trow = tex->pixels + (int32_t)ty * tw;
        const bool corner_row = radius > 0 && (y < r.y + radius || y >= r.y + r.h - radius);
        int tx = (((x0 - origin_x) % tw) + tw) % tw;
        for (int x = x0; x < x1; ++x) {
            const uint32_t cov = corner_row ? round_cov(r, radius, x, y) : 255;
            row[x] = blend565(row[x], trow[tx], cov);
            if (++tx == tw) {
                tx = 0;
            }
        }
    }
}

/** 旋转后矩形尺寸 */
static inline void rotated_size(int w, int h, gfx_rot_t rot, int *rw, int *rh)
{
    const bool swap = rot == GFX_ROT_90 || rot == GFX_ROT_270;
    *rw = swap ? h : w;
    *rh = swap ? w : h;
}

void gfx_mask(gfx_canvas_t *c, const uint8_t *mask, int width, int height, int stride, int x, int y,
              gfx_rot_t rot, uint32_t color, uint8_t alpha)
{
    int rw, rh;
    rotated_size(width, height, rot, &rw, &rh);
    int x0, y0, x1, y1;
    if (!mask || alpha == 0 || !clip_span(c, gfx_rect(x, y, rw, rh), &x0, &y0, &x1, &y1)) {
        return;
    }
    const uint16_t s = gfx_rgb565(color);
    for (int dy = y0; dy < y1; ++dy) {
        uint16_t *row = pixel_at(c, 0, dy);
        const int j = dy - y;
        for (int dx = x0; dx < x1; ++dx) {
            const int i = dx - x;
            int u, v;
            /* 目标局部 (i, j) 对应遮罩 (u, v)：内容顺时针旋转 rot */
            switch (rot) {
            case GFX_ROT_90:
                u = j;
                v = height - 1 - i;
                break;
            case GFX_ROT_180:
                u = width - 1 - i;
                v = height - 1 - j;
                break;
            case GFX_ROT_270:
                u = width - 1 - j;
                v = i;
                break;
            default:
                u = i;
                v = j;
                break;
            }
            const uint32_t m = mask[(int32_t)v * stride + u];
            if (m) {
                row[dx] = blend565(row[dx], s, alpha == 255 ? m : div255(m * alpha));
            }
        }
    }
}

void gfx_icon(gfx_canvas_t *c, const gfx_image_t *icon, int cx, int cy, gfx_rot_t rot, uint32_t color, uint8_t alpha)
{
    int rw, rh;
    rotated_size(icon->width, icon->height, rot, &rw, &rh);
    gfx_mask(c, icon->alpha, icon->width, icon->height, icon->width, cx - rw / 2, cy - rh / 2, rot, color, alpha);
}

/* ---------------------------------------------------------------- 文字 */

const char *gfx_utf8_next(const char *s, uint32_t *ret_codepoint)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t cp = p[0];
    int extra = 0;
    if (cp >= 0xF0 && cp < 0xF8) {
        cp &= 0x07;
        extra = 3;
    } else if (cp >= 0xE0) {
        cp &= 0x0F;
        extra = 2;
    } else if (cp >= 0xC0) {
        cp &= 0x1F;
        extra = 1;
    }
    for (int i = 1; i <= extra; ++i) {
        if ((p[i] & 0xC0) != 0x80) {
            *ret_codepoint = p[0];
            return s + 1;
        }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *ret_codepoint = cp;
    return s + 1 + extra;
}

const gfx_glyph_t *gfx_font_find(const gfx_font_t *font, uint32_t codepoint)
{
    if (!font) {
        return NULL;
    }
    int lo = 0;
    int hi = (int)font->count - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uint32_t cp = font->glyphs[mid].codepoint;
        if (cp == codepoint) {
            return &font->glyphs[mid];
        }
        if (cp < codepoint) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

/** 在主字体找不到时退到备用字体；都没有则返回 NULL */
static const gfx_glyph_t *style_glyph(const gfx_text_style_t *style, uint32_t cp, const gfx_font_t **ret_font)
{
    const gfx_glyph_t *g = gfx_font_find(style->font, cp);
    *ret_font = style->font;
    if (!g && style->fallback) {
        g = gfx_font_find(style->fallback, cp);
        *ret_font = style->fallback;
    }
    return g;
}

int gfx_text_width_q4(const gfx_text_style_t *style, const char *utf8)
{
    int width = 0;
    bool first = true;
    while (utf8 && *utf8) {
        uint32_t cp;
        utf8 = gfx_utf8_next(utf8, &cp);
        const gfx_font_t *font;
        const gfx_glyph_t *g = style_glyph(style, cp, &font);
        if (!g) {
            continue;
        }
        if (!first) {
            width += style->tracking_q4;
        }
        width += g->advance_q4;
        first = false;
    }
    return width;
}

/** 绘制文字；off_u/off_v 为"用户方向"下的整体偏移（刻字高光用） */
static void draw_text(gfx_canvas_t *c, const gfx_text_style_t *style, int ax, int ay, const char *utf8, int off_u,
                      int off_v, uint32_t color, uint8_t alpha)
{
    if (!style->font || !utf8 || alpha == 0) {
        return;
    }
    const int width = gfx_text_width_q4(style, utf8);
    int pen = style->align == GFX_ALIGN_CENTER ? -width / 2 : (style->align == GFX_ALIGN_RIGHT ? -width : 0);
    const int baseline = (style->font->ascent + 1) / 2;
    while (*utf8) {
        uint32_t cp;
        utf8 = gfx_utf8_next(utf8, &cp);
        const gfx_font_t *font;
        const gfx_glyph_t *g = style_glyph(style, cp, &font);
        if (!g) {
            continue;
        }
        if (g->width && g->height) {
            const int gx = ((pen + 8) >> 4) + g->left + off_u;
            const int gy = baseline + g->top + off_v;
            int sx, sy;
            switch (style->rot) {
            case GFX_ROT_90:
                sx = ax - gy - g->height;
                sy = ay + gx;
                break;
            case GFX_ROT_180:
                sx = ax - gx - g->width;
                sy = ay - gy - g->height;
                break;
            case GFX_ROT_270:
                sx = ax + gy;
                sy = ay - gx - g->width;
                break;
            default:
                sx = ax + gx;
                sy = ay + gy;
                break;
            }
            gfx_mask(c, font->bitmap + g->offset, g->width, g->height, g->width, sx, sy, style->rot, color, alpha);
        }
        pen += g->advance_q4 + style->tracking_q4;
    }
}

void gfx_text(gfx_canvas_t *c, const gfx_text_style_t *style, int x, int y, const char *utf8)
{
    draw_text(c, style, x, y, utf8, 0, 0, style->color, style->alpha);
}

void gfx_text_engraved(gfx_canvas_t *c, const gfx_text_style_t *style, int x, int y, const char *utf8,
                       uint32_t highlight, uint8_t highlight_alpha)
{
    if (highlight_alpha) {
        draw_text(c, style, x, y, utf8, 0, 1, highlight, highlight_alpha);
    }
    draw_text(c, style, x, y, utf8, 0, 0, style->color, style->alpha);
}

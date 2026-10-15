/*
 * film_yuv 与条带版白平衡统计的测试：
 *   1. 内存布局与 ESP 硬件编码器的 YUV420（OUYY_EVYY）一致（对照 ESP-IDF h264 示例里的取像素公式）
 *   2. RGB → YUV → RGB 的往返误差在 4:2:0 应有的范围内（平坦色块几乎无损，只在色边处有色度模糊）
 *   3. 矩形写入的对齐检查、奇数尺寸的边缘处理、分块写入与整幅写入一致
 *   4. 条带版白平衡统计与整幅统计逐位一致
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_balance.h"
#include "film_yuv.h"

static int failures;
#define CHECK(cond, ...)                                                                                              \
    do {                                                                                                              \
        if (!(cond)) {                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                               \
            printf(__VA_ARGS__);                                                                                      \
            printf("\n");                                                                                             \
            ++failures;                                                                                               \
        }                                                                                                             \
    } while (0)

static uint32_t rng_state = 12345;
static uint32_t rnd(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* 平滑渐变 + 噪声（noise_mask 为噪声幅度掩码，0 表示无噪声） */
static unsigned noise_mask = 7;
static void make_image(uint8_t *rgb, int w, int h)
{
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (uint8_t)((x * 255 / (w > 1 ? w - 1 : 1) + (rnd() & noise_mask)) & 255);
            p[1] = (uint8_t)((y * 255 / (h > 1 ? h - 1 : 1) + (rnd() & noise_mask)) & 255);
            p[2] = (uint8_t)(((x + y) * 255 / (w + h) + (rnd() & noise_mask)) & 255);
        }
    }
}

/* ESP-IDF examples/peripherals/h264 的 ESP_H264_RAW_FMT_O_UYY_E_VYY 取像素公式（独立实现，作布局基准） */
static void reference_pixel(const uint8_t *data, int width, int x, int y, int *ret_y, int *ret_u, int *ret_v)
{
    const uint8_t *uyy = data + (size_t)(y >> 1) * width * 3 + (size_t)(x >> 1) * 3;
    const uint8_t *vyy = uyy + (size_t)width * 3 / 2;
    *ret_y = (y & 1) ? vyy[1 + (x & 1)] : uyy[1 + (x & 1)];
    *ret_u = uyy[0];
    *ret_v = vyy[0];
}

static void test_layout(void)
{
    enum { W = 16, H = 8 };
    uint8_t rgb[W * H * 3];
    uint8_t yuv[W * H * 3 / 2];
    make_image(rgb, W, H);
    CHECK(film_yuv_bytes(W, H) == sizeof(yuv), "bytes");
    CHECK(film_yuv_put_rgb(yuv, W, H, 0, 0, W, H, rgb, W * 3) == 0, "put");
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int ry, ru, rv;
            reference_pixel(yuv, W, x, y, &ry, &ru, &rv);
            uint8_t ey, eu, ev;
            const uint8_t *p = rgb + ((size_t)y * W + x) * 3;
            film_yuv_from_rgb(p[0], p[1], p[2], &ey, &eu, &ev);
            CHECK(ry == ey, "Y(%d,%d): %d vs %d", x, y, ry, ey);
            CHECK(film_yuv_luma(yuv, W, x, y) == ey, "luma accessor");
            /* 色度是 2×2 平均：用块内 4 个像素重算 */
            int us = 0, vs = 0;
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const uint8_t *q = rgb + ((size_t)((y & ~1) + j) * W + ((x & ~1) + i)) * 3;
                    uint8_t yy, uu, vv;
                    film_yuv_from_rgb(q[0], q[1], q[2], &yy, &uu, &vv);
                    us += uu;
                    vs += vv;
                }
            }
            CHECK(ru == ((us + 2) >> 2) && rv == ((vs + 2) >> 2), "UV(%d,%d)", x, y);
        }
    }
}

static void test_roundtrip(void)
{
    /* 纯色块：除舍入外无损 */
    const uint8_t colors[][3] = { { 0, 0, 0 }, { 255, 255, 255 }, { 200, 30, 40 }, { 20, 180, 60 }, { 30, 40, 220 } };
    for (size_t i = 0; i < sizeof(colors) / sizeof(colors[0]); ++i) {
        uint8_t rgb[8 * 8 * 3], yuv[8 * 8 * 3 / 2], back[8 * 8 * 3];
        for (int k = 0; k < 64; ++k) {
            memcpy(rgb + k * 3, colors[i], 3);
        }
        film_yuv_put_rgb(yuv, 8, 8, 0, 0, 8, 8, rgb, 24);
        film_yuv_get_rgb(yuv, 8, 8, 0, 0, 8, 8, back, 24);
        for (int k = 0; k < 64 * 3; ++k) {
            const int d = abs(back[k] - rgb[k]);
            CHECK(d <= 3, "solid color %zu channel %d off by %d", i, k % 3, d);
        }
    }
    /* 平滑照片：平均误差很小 */
    enum { W = 64, H = 48 };
    static uint8_t rgb[W * H * 3], yuv[W * H * 3 / 2], back[W * H * 3];
    noise_mask = 0;
    make_image(rgb, W, H);
    noise_mask = 7;
    film_yuv_put_rgb(yuv, W, H, 0, 0, W, H, rgb, W * 3);
    film_yuv_get_rgb(yuv, W, H, 0, 0, W, H, back, W * 3);
    long sum = 0;
    for (int k = 0; k < W * H * 3; ++k) {
        sum += abs(back[k] - rgb[k]);
    }
    CHECK(sum / (W * H * 3) <= 2, "photo-like mean abs error %ld", sum / (W * H * 3));
}

static void test_tiles_and_edges(void)
{
    /* 分块写入（模拟 8 列的解码块）与整幅写入一致；奇数尺寸 */
    const int sizes[][2] = { { 64, 48 }, { 63, 47 }, { 17, 9 }, { 2, 2 }, { 1, 1 } };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const int w = sizes[s][0], h = sizes[s][1];
        uint8_t *rgb = malloc((size_t)w * h * 3);
        uint8_t *a = calloc(1, film_yuv_bytes(w, h)), *b = calloc(1, film_yuv_bytes(w, h));
        make_image(rgb, w, h);
        CHECK(film_yuv_put_rgb(a, w, h, 0, 0, w, h, rgb, (size_t)w * 3) == 0, "whole put %dx%d", w, h);
        for (int x = 0; x < w; x += 8) {
            const int n = w - x < 8 ? w - x : 8;
            CHECK(film_yuv_put_rgb(b, w, h, x, 0, n, h, rgb + x * 3, (size_t)w * 3) == 0, "tile put %dx%d x=%d", w, h,
                  x);
        }
        CHECK(memcmp(a, b, film_yuv_bytes(w, h)) == 0, "tiles differ from whole at %dx%d", w, h);
        free(rgb), free(a), free(b);
    }
    /* 对齐与越界要被拒绝 */
    uint8_t rgb[16 * 16 * 3] = { 0 }, yuv[16 * 16 * 3 / 2];
    CHECK(film_yuv_put_rgb(yuv, 16, 16, 1, 0, 4, 4, rgb, 48) != 0, "odd x accepted");
    CHECK(film_yuv_put_rgb(yuv, 16, 16, 0, 1, 4, 4, rgb, 48) != 0, "odd y accepted");
    CHECK(film_yuv_put_rgb(yuv, 16, 16, 0, 0, 3, 4, rgb, 48) != 0, "odd w mid-image accepted");
    CHECK(film_yuv_put_rgb(yuv, 16, 16, 12, 0, 8, 4, rgb, 48) != 0, "overflow accepted");
    CHECK(film_yuv_get_rgb(yuv, 16, 16, 10, 10, 8, 8, rgb, 48) != 0, "get overflow accepted");
}

static void test_balance_strips(void)
{
    const int sizes[][2] = { { 640, 480 }, { 333, 517 }, { 2048, 1536 } };
    const int strips[] = { 1, 3, 4, 16, 37, 211 };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        const int w = sizes[s][0], h = sizes[s][1];
        uint8_t *rgb = malloc((size_t)w * h * 3);
        make_image(rgb, w, h);
        film_tone_stats_t whole;
        static film_tone_hist_t whole_hist, strip_hist;
        film_balance_measure(rgb, (size_t)w * 3, w, h, &whole, &whole_hist);
        for (size_t k = 0; k < sizeof(strips) / sizeof(strips[0]); ++k) {
            film_tone_accum_t acc;
            film_balance_accum_begin(&acc, &strip_hist);
            for (int y = 0; y < h; y += strips[k]) {
                const int n = h - y < strips[k] ? h - y : strips[k];
                film_balance_accum_rows(&acc, rgb + (size_t)y * w * 3, (size_t)w * 3, w, y, n);
            }
            film_tone_stats_t st;
            film_balance_accum_finish(&acc, &st);
            CHECK(st.samples == whole.samples && st.valid == whole.valid, "%dx%d strip %d samples %u vs %u", w, h,
                  strips[k], st.samples, whole.samples);
            CHECK(memcmp(&st.mean, &whole.mean, sizeof(st.mean)) == 0 || (
                      /* 浮点累加顺序相同，应逐位一致 */ 0),
                  "%dx%d strip %d mean differs", w, h, strips[k]);
            CHECK(memcmp(&strip_hist, &whole_hist, sizeof(whole_hist)) == 0, "%dx%d strip %d hist differs", w, h,
                  strips[k]);
        }
        free(rgb);
    }
}

int main(void)
{
    test_layout();
    test_roundtrip();
    test_tiles_and_edges();
    test_balance_strips();
    printf(failures ? "film_yuv: %d failure(s)\n" : "film_yuv: all passed\n", failures);
    return failures ? 1 : 0;
}

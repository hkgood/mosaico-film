/*
 * 分条带冲洗必须与整图冲洗逐位一致：每款胶卷 × 漏光/颗粒 × 通道顺序 × 并行 × 图像尺寸 × 条带高度，
 * 条带高度故意取不被网格因子整除的值（红晕网格行会跨条带），并倒序处理条带验证与顺序无关。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_filter.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define MAX_SIDE 2048

/* 平滑渐变 + 伪随机纹理 + 一块过曝高光（让红晕有东西可发） */
static void fill_pattern(uint8_t *pixels, int width, int height)
{
    uint32_t state = 777;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint8_t *px = pixels + ((size_t)y * width + x) * 3;
            state = state * 1664525u + 1013904223u;
            const int noise = (int)(state >> 28) - 8;
            int r = x * 255 / (width - 1), g = y * 255 / (height - 1), b = (r + g) / 2;
            if (x > width / 3 && x < width / 2 && y > height / 4 && y < height / 3) {
                r = g = b = 250;   /* 高光块 */
            }
            px[0] = (uint8_t)((r + noise) & 0xFF);
            px[1] = (uint8_t)((g + noise) & 0xFF);
            px[2] = (uint8_t)((b + noise) & 0xFF);
        }
    }
}

static film_image_t view(uint8_t *pixels, int width, int y0, int rows, film_channel_order_t order)
{
    return (film_image_t) {
        .pixels = pixels + (size_t)y0 * width * 3, .width = (uint16_t)width, .height = (uint16_t)rows,
        .stride = (size_t)width * 3, .order = order,
    };
}

typedef struct {
    int width, height;
    int strip;
    film_id_t film;
    bool leak, grain, bgr, parallel, reverse;
    float ev;
} scenario_t;

static void run_scenario(const scenario_t *sc)
{
    const size_t bytes = (size_t)sc->width * sc->height * 3;
    uint8_t *src = malloc(bytes), *whole = malloc(bytes), *strips = malloc(bytes);
    CHECK(src && whole && strips);
    fill_pattern(src, sc->width, sc->height);
    memcpy(whole, src, bytes);
    memcpy(strips, src, bytes);
    const film_channel_order_t order = sc->bgr ? FILM_ORDER_BGR : FILM_ORDER_RGB;

    film_filter_handle_t filter;
    const film_filter_config_t config = { .max_width = MAX_SIDE, .max_height = MAX_SIDE, .parallel = sc->parallel };
    CHECK(film_filter_create(&config, &filter) == ESP_OK);
    const film_develop_params_t params = {
        .film = sc->film, .seed = 4242, .light_leak = sc->leak, .grain = sc->grain, .exposure_ev = sc->ev,
    };

    /* 整图 */
    const film_image_t full = view(whole, sc->width, 0, sc->height, order);
    CHECK(film_filter_develop(filter, &params, &full) == ESP_OK);

    /* 分条带（同一个句柄，顺便验证两种方式交替使用互不污染） */
    CHECK(film_filter_strip_begin(filter, &params, (uint16_t)sc->width, (uint16_t)sc->height, order) == ESP_OK);
    const int count = (sc->height + sc->strip - 1) / sc->strip;
    const film_strip_pre_pass_t pre = film_filter_strip_pre_pass(filter);
    if (pre != FILM_STRIP_PRE_PASS_NONE) {
        for (int i = 0; i < count; ++i) {
            const int k = sc->reverse ? count - 1 - i : i;
            const int y0 = k * sc->strip;
            const int rows = y0 + sc->strip <= sc->height ? sc->strip : sc->height - y0;
            const film_image_t strip = view(strips, sc->width, y0, rows, order);
            CHECK(film_filter_strip_pre(filter, &strip, (uint16_t)y0) == ESP_OK);
        }
        CHECK(film_filter_strip_resolve(filter) == ESP_OK);
    }
    for (int i = 0; i < count; ++i) {
        const int k = sc->reverse ? count - 1 - i : i;
        const int y0 = k * sc->strip;
        const int rows = y0 + sc->strip <= sc->height ? sc->strip : sc->height - y0;
        const film_image_t strip = view(strips, sc->width, y0, rows, order);
        CHECK(film_filter_strip_finish(filter, &strip, (uint16_t)y0) == ESP_OK);
    }
    film_filter_strip_end(filter);

    if (memcmp(whole, strips, bytes) != 0) {
        size_t at = 0;
        while (whole[at] == strips[at]) {
            ++at;
        }
        fprintf(stderr, "MISMATCH film=%d %dx%d strip=%d leak=%d grain=%d bgr=%d par=%d rev=%d ev=%.1f at byte %zu "
                "(y=%zu)\n", (int)sc->film, sc->width, sc->height, sc->strip, sc->leak, sc->grain, sc->bgr,
                sc->parallel, sc->reverse, (double)sc->ev, at, at / ((size_t)sc->width * 3));
        exit(1);
    }
    film_filter_delete(filter);
    free(src);
    free(whole);
    free(strips);
}

static void test_equivalence(void)
{
    static const struct { int w, h; } small_sizes[] = { { 640, 480 }, { 333, 517 }, { 480, 360 } };
    static const int strip_heights[] = { 1, 16, 37, 64, 211 };
    int cases = 0;
    for (int film = 0; film < FILM_ID_COUNT; ++film) {
        for (size_t s = 0; s < sizeof(small_sizes) / sizeof(small_sizes[0]); ++s) {
            for (size_t h = 0; h < sizeof(strip_heights) / sizeof(strip_heights[0]); ++h) {
                const int variant = (film + (int)s + (int)h) % 4;   /* 在组合间轮换，覆盖所有开关又不爆炸 */
                scenario_t sc = {
                    .width = small_sizes[s].w, .height = small_sizes[s].h, .strip = strip_heights[h],
                    .film = (film_id_t)film, .leak = variant & 1, .grain = true, .bgr = (variant >> 1) & 1,
                    .parallel = (h & 1) != 0, .reverse = (s & 1) != 0, .ev = (h == 2) ? 0.7f : (h == 3 ? -0.5f : 0.0f),
                };
                run_scenario(&sc);
                ++cases;
            }
        }
    }
    /* 全尺寸：成片的真实几何（含竖拿 1536×2048），条带 64 行与一个不整除的高度 */
    for (int film = 0; film < FILM_ID_COUNT; ++film) {
        const scenario_t landscape = { .width = 2048, .height = 1536, .strip = 64, .film = (film_id_t)film,
                                       .leak = film & 1, .grain = true, .bgr = (film >> 1) & 1, .parallel = true };
        run_scenario(&landscape);
        const scenario_t portrait = { .width = 1536, .height = 2048, .strip = 211, .film = (film_id_t)film,
                                      .leak = !(film & 1), .grain = film != 3, .reverse = true };
        run_scenario(&portrait);
        cases += 2;
    }
    printf("strip == whole: %d scenarios identical\n", cases);
}

static void test_session_errors(void)
{
    film_filter_handle_t filter;
    const film_filter_config_t config = { .max_width = 256, .max_height = 256 };
    CHECK(film_filter_create(&config, &filter) == ESP_OK);
    uint8_t pixels[64 * 16 * 3] = { 0 };
    film_image_t strip = view(pixels, 64, 0, 16, FILM_ORDER_RGB);
    const film_develop_params_t params = { .film = FILM_ID_GOLD };

    /* 没有 begin */
    CHECK(film_filter_strip_finish(filter, &strip, 0) == ESP_ERR_INVALID_STATE);
    CHECK(film_filter_strip_pre(filter, &strip, 0) == ESP_ERR_INVALID_STATE);
    CHECK(film_filter_strip_resolve(filter) == ESP_ERR_INVALID_STATE);
    CHECK(film_filter_strip_pre_pass(filter) == FILM_STRIP_PRE_PASS_NONE);

    /* begin 参数 */
    CHECK(film_filter_strip_begin(NULL, &params, 64, 64, FILM_ORDER_RGB) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_strip_begin(filter, NULL, 64, 64, FILM_ORDER_RGB) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_strip_begin(filter, &params, 0, 64, FILM_ORDER_RGB) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_strip_begin(filter, &params, 300, 64, FILM_ORDER_RGB) == ESP_ERR_INVALID_SIZE);
    CHECK(film_filter_strip_begin(filter, &params, 64, 300, FILM_ORDER_RGB) == ESP_ERR_INVALID_SIZE);
    const film_develop_params_t bad_film = { .film = FILM_ID_COUNT };
    CHECK(film_filter_strip_begin(filter, &bad_film, 64, 64, FILM_ORDER_RGB) == ESP_ERR_INVALID_ARG);

    /* 条带与会话不符：宽度、通道顺序、越过图像底部 */
    CHECK(film_filter_strip_begin(filter, &params, 64, 48, FILM_ORDER_RGB) == ESP_OK);
    film_image_t wide = view(pixels, 32, 0, 16, FILM_ORDER_RGB);
    CHECK(film_filter_strip_finish(filter, &wide, 0) == ESP_ERR_INVALID_ARG);
    film_image_t wrong_order = view(pixels, 64, 0, 16, FILM_ORDER_BGR);
    CHECK(film_filter_strip_finish(filter, &wrong_order, 0) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_strip_finish(filter, &strip, 40) == ESP_ERR_INVALID_ARG);   /* 40 + 16 > 48 */
    CHECK(film_filter_strip_finish(filter, &strip, 32) == ESP_OK);                /* 恰好到底 */

    /* end 之后会话失效 */
    film_filter_strip_end(filter);
    CHECK(film_filter_strip_finish(filter, &strip, 0) == ESP_ERR_INVALID_STATE);
    film_filter_delete(filter);
}

int main(void)
{
    test_session_errors();
    test_equivalence();
    printf("film_filter_strip_test: ok\n");
    return 0;
}

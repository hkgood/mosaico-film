/*
 * film_filter 的主机单元测试：接口参数检查、可复现性、行跨度、边界尺寸。
 * 与 Python 原型的逐像素对比由对比脚本完成，这里不重复。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_filter.h"

static int s_failures;

#define CHECK(cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            s_failures++; \
        } \
    } while (0)

/* 可复现的测试图：平滑渐变叠加一点伪随机纹理，覆盖暗部、中间调和高光 */
static void fill_test_pattern(uint8_t *pixels, int width, int height, size_t stride)
{
    uint32_t state = 12345;
    for (int y = 0; y < height; y++) {
        uint8_t *px = pixels + (size_t)y * stride;
        for (int x = 0; x < width; x++, px += 3) {
            state = state * 1664525u + 1013904223u;
            int noise = (int)(state >> 28) - 8;
            int base = (x * 255) / (width > 1 ? width - 1 : 1);
            int v = (y * 255) / (height > 1 ? height - 1 : 1);
            px[0] = (uint8_t)((base + noise) & 0xFF);
            px[1] = (uint8_t)((v + noise) & 0xFF);
            px[2] = (uint8_t)(((base + v) / 2) & 0xFF);
        }
    }
}

static film_image_t make_image(uint8_t *pixels, int width, int height, size_t stride)
{
    return (film_image_t) {
        .pixels = pixels, .width = (uint16_t)width, .height = (uint16_t)height, .stride = stride,
    };
}

static void test_arguments(void)
{
    film_filter_handle_t filter = NULL;
    film_filter_config_t bad = { .max_width = 0, .max_height = 10 };
    CHECK(film_filter_create(NULL, &filter) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_create(&bad, &filter) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_delete(NULL) == ESP_ERR_INVALID_ARG);

    film_filter_config_t config = { .max_width = 8, .max_height = 8 };
    CHECK(film_filter_create(&config, &filter) == ESP_OK);
    uint8_t pixels[9 * 9 * 3] = { 0 };
    film_develop_params_t params = { .film = FILM_ID_GOLD, .grain = true };

    film_image_t too_big = make_image(pixels, 9, 9, 9 * 3);
    CHECK(film_filter_develop(filter, &params, &too_big) == ESP_ERR_INVALID_SIZE);
    film_image_t bad_stride = make_image(pixels, 8, 8, 8 * 3 - 1);
    CHECK(film_filter_develop(filter, &params, &bad_stride) == ESP_ERR_INVALID_ARG);
    film_image_t ok = make_image(pixels, 8, 8, 8 * 3);
    params.film = FILM_ID_COUNT;
    CHECK(film_filter_develop(filter, &params, &ok) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_develop(filter, NULL, &ok) == ESP_ERR_INVALID_ARG);
    CHECK(film_filter_delete(filter) == ESP_OK);
}

static void test_keys(void)
{
    for (int i = 0; i < FILM_ID_COUNT; i++) {
        const char *key = film_filter_get_key((film_id_t)i);
        film_id_t found = FILM_ID_COUNT;
        CHECK(key != NULL);
        CHECK(film_filter_find_by_key(key, &found) == ESP_OK && found == (film_id_t)i);
    }
    film_id_t unused;
    CHECK(film_filter_get_key(FILM_ID_COUNT) == NULL);
    CHECK(film_filter_find_by_key("kodak", &unused) == ESP_ERR_NOT_FOUND);
    CHECK(film_filter_find_by_key(NULL, &unused) == ESP_ERR_INVALID_ARG);
}

/* 同一种子结果完全一致；换种子颗粒随之改变 */
static void test_determinism(void)
{
    enum { W = 64, H = 48 };
    size_t size = W * H * 3;
    uint8_t *a = malloc(size), *b = malloc(size);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H };
    CHECK(a && b && film_filter_create(&config, &filter) == ESP_OK);

    film_develop_params_t params = { .film = FILM_ID_NIGHT, .seed = 7, .light_leak = true, .grain = true };
    film_image_t ia = make_image(a, W, H, W * 3), ib = make_image(b, W, H, W * 3);
    fill_test_pattern(a, W, H, W * 3);
    fill_test_pattern(b, W, H, W * 3);
    CHECK(film_filter_develop(filter, &params, &ia) == ESP_OK);
    CHECK(film_filter_develop(filter, &params, &ib) == ESP_OK);
    CHECK(memcmp(a, b, size) == 0);

    fill_test_pattern(b, W, H, W * 3);
    params.seed = 8;
    CHECK(film_filter_develop(filter, &params, &ib) == ESP_OK);
    CHECK(memcmp(a, b, size) != 0);

    film_filter_delete(filter);
    free(a);
    free(b);
}

/* 带行跨度的图像：结果与紧凑图一致，行尾填充字节不被改动 */
static void test_stride(void)
{
    enum { W = 37, H = 21, PAD = 5 };
    size_t stride = W * 3 + PAD;
    uint8_t *compact = malloc(W * H * 3), *padded = malloc(stride * H);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H };
    CHECK(compact && padded && film_filter_create(&config, &filter) == ESP_OK);

    for (int film = 0; film < FILM_ID_COUNT; film++) {
        film_develop_params_t params = { .film = (film_id_t)film, .seed = 3, .light_leak = true, .grain = true };
        fill_test_pattern(compact, W, H, W * 3);
        memset(padded, 0xA5, stride * H);
        fill_test_pattern(padded, W, H, stride);
        film_image_t ic = make_image(compact, W, H, W * 3), ip = make_image(padded, W, H, stride);
        CHECK(film_filter_develop(filter, &params, &ic) == ESP_OK);
        CHECK(film_filter_develop(filter, &params, &ip) == ESP_OK);
        for (int y = 0; y < H; y++) {
            CHECK(memcmp(compact + (size_t)y * W * 3, padded + (size_t)y * stride, W * 3) == 0);
            for (int p = 0; p < PAD; p++) {
                CHECK(padded[(size_t)y * stride + W * 3 + p] == 0xA5);
            }
        }
    }
    film_filter_delete(filter);
    free(compact);
    free(padded);
}

/* 黑白胶卷三通道相等；像素风最多 32 种颜色 */
static void test_film_properties(void)
{
    enum { W = 120, H = 90 };
    uint8_t *pixels = malloc(W * H * 3);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H };
    CHECK(pixels && film_filter_create(&config, &filter) == ESP_OK);
    film_image_t image = make_image(pixels, W, H, W * 3);

    film_develop_params_t params = { .film = FILM_ID_BW, .seed = 1, .grain = true };
    fill_test_pattern(pixels, W, H, W * 3);
    CHECK(film_filter_develop(filter, &params, &image) == ESP_OK);
    for (int i = 0; i < W * H; i++) {
        CHECK(pixels[i * 3] == pixels[i * 3 + 1] && pixels[i * 3] == pixels[i * 3 + 2]);
    }

    params.film = FILM_ID_PIXEL;
    fill_test_pattern(pixels, W, H, W * 3);
    CHECK(film_filter_develop(filter, &params, &image) == ESP_OK);
    uint32_t seen[64] = { 0 };
    int unique = 0;
    for (int i = 0; i < W * H && unique <= 32; i++) {
        uint32_t rgb = ((uint32_t)pixels[i * 3] << 16) | ((uint32_t)pixels[i * 3 + 1] << 8) | pixels[i * 3 + 2];
        int j = 0;
        while (j < unique && seen[j] != rgb) {
            j++;
        }
        if (j == unique) {
            seen[unique++] = rgb;
        }
    }
    CHECK(unique <= 32);

    film_filter_delete(filter);
    free(pixels);
}

/* 极端尺寸（1 像素、细长条、网格不整除）下所有胶卷都能跑完 */
static void test_edge_sizes(void)
{
    static const int sizes[][2] = { { 1, 1 }, { 2, 1 }, { 1, 97 }, { 3, 7 }, { 481, 17 }, { 161, 160 },
                                    { 640, 480 } };
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = 640, .max_height = 480 };
    uint8_t *pixels = malloc(640 * 480 * 3);
    CHECK(pixels && film_filter_create(&config, &filter) == ESP_OK);
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int w = sizes[s][0], h = sizes[s][1];
        for (int film = 0; film < FILM_ID_COUNT; film++) {
            film_develop_params_t params = { .film = (film_id_t)film, .seed = 9, .light_leak = true, .grain = true };
            fill_test_pattern(pixels, w, h, (size_t)w * 3);
            film_image_t image = make_image(pixels, w, h, (size_t)w * 3);
            CHECK(film_filter_develop(filter, &params, &image) == ESP_OK);
        }
    }
    film_filter_delete(filter);
    free(pixels);
}

/* 用一个全新的句柄冲洗，作为缓存与并行测试的参照 */
static esp_err_t develop_fresh(const film_develop_params_t *params, const film_image_t *image, bool parallel)
{
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = image->width, .max_height = image->height, .parallel = parallel };
    esp_err_t err = film_filter_create(&config, &filter);
    if (err == ESP_OK) {
        err = film_filter_develop(filter, params, image);
        film_filter_delete(filter);
    }
    return err;
}

/* 双线程冲洗与单线程逐字节一致，包括行数太少不拆分、红晕网格对齐拆分的情况 */
static void test_parallel_matches_serial(void)
{
    static const int sizes[][2] = { { 1, 1 }, { 3, 7 }, { 5, 16 }, { 481, 17 }, { 161, 160 }, { 641, 377 } };
    enum { MAX_W = 641, MAX_H = 377 };
    uint8_t *serial = malloc(MAX_W * MAX_H * 3), *parallel = malloc(MAX_W * MAX_H * 3);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = MAX_W, .max_height = MAX_H, .parallel = true };
    CHECK(serial && parallel && film_filter_create(&config, &filter) == ESP_OK);

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        int w = sizes[s][0], h = sizes[s][1];
        size_t size = (size_t)w * h * 3;
        film_image_t is = make_image(serial, w, h, (size_t)w * 3), ip = make_image(parallel, w, h, (size_t)w * 3);
        for (int film = 0; film < FILM_ID_COUNT; film++) {
            for (int leak = 0; leak < 2; leak++) {
                film_develop_params_t params = {
                    .film = (film_id_t)film, .seed = 11, .light_leak = leak != 0, .grain = true,
                };
                fill_test_pattern(serial, w, h, (size_t)w * 3);
                fill_test_pattern(parallel, w, h, (size_t)w * 3);
                CHECK(develop_fresh(&params, &is, false) == ESP_OK);
                CHECK(film_filter_develop(filter, &params, &ip) == ESP_OK);
                CHECK(memcmp(serial, parallel, size) == 0);
            }
        }
    }
    film_filter_delete(filter);
    free(serial);
    free(parallel);
}

/* 同一个句柄轮流切换胶卷/尺寸/种子/漏光，结果必须与每次都用新句柄相同（查找表缓存不串） */
static void test_table_cache(void)
{
    static const struct {
        film_id_t film;
        int width;
        int height;
        uint32_t seed;
        bool leak;
    } steps[] = {
        { FILM_ID_GOLD, 64, 48, 1, true },  { FILM_ID_GOLD, 64, 48, 1, true },  { FILM_ID_GOLD, 64, 48, 2, true },
        { FILM_ID_GOLD, 48, 64, 2, true },  { FILM_ID_NIGHT, 48, 64, 2, true }, { FILM_ID_NIGHT, 48, 64, 2, false },
        { FILM_ID_PIXEL, 48, 64, 2, true }, { FILM_ID_NIGHT, 48, 64, 2, true }, { FILM_ID_BW, 30, 20, 5, false },
        { FILM_ID_BW, 30, 20, 5, true },    { FILM_ID_BW, 64, 48, 5, true },    { FILM_ID_GOLD, 64, 48, 1, true },
    };
    enum { MAX_W = 64, MAX_H = 64 };
    uint8_t *reused = malloc(MAX_W * MAX_H * 3), *fresh = malloc(MAX_W * MAX_H * 3);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = MAX_W, .max_height = MAX_H };
    CHECK(reused && fresh && film_filter_create(&config, &filter) == ESP_OK);

    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        int w = steps[i].width, h = steps[i].height;
        film_develop_params_t params = {
            .film = steps[i].film, .seed = steps[i].seed, .light_leak = steps[i].leak, .grain = true,
        };
        film_image_t ir = make_image(reused, w, h, (size_t)w * 3), ifr = make_image(fresh, w, h, (size_t)w * 3);
        fill_test_pattern(reused, w, h, (size_t)w * 3);
        fill_test_pattern(fresh, w, h, (size_t)w * 3);
        CHECK(film_filter_develop(filter, &params, &ir) == ESP_OK);
        CHECK(develop_fresh(&params, &ifr, false) == ESP_OK);
        if (memcmp(reused, fresh, (size_t)w * h * 3) != 0) {
            fprintf(stderr, "table cache mismatch at step %zu\n", i);
            s_failures++;
        }
    }
    film_filter_delete(filter);
    free(reused);
    free(fresh);
}

static void swap_rb(uint8_t *pixels, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint8_t r = pixels[i * 3];
        pixels[i * 3] = pixels[i * 3 + 2];
        pixels[i * 3 + 2] = r;
    }
}

/* BGR 内存顺序的结果与 RGB 逐字节互为红蓝交换（所有胶卷，含并行） */
static void test_bgr_order(void)
{
    enum { W = 161, H = 120 };
    size_t size = W * H * 3;
    uint8_t *rgb = malloc(size), *bgr = malloc(size);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H, .parallel = true };
    CHECK(rgb && bgr && film_filter_create(&config, &filter) == ESP_OK);

    for (int film = 0; film < FILM_ID_COUNT; film++) {
        film_develop_params_t params = {
            .film = (film_id_t)film, .seed = 4, .light_leak = true, .grain = true, .exposure_ev = 0.7f,
        };
        fill_test_pattern(rgb, W, H, W * 3);
        fill_test_pattern(bgr, W, H, W * 3);
        swap_rb(bgr, (size_t)W * H);
        film_image_t ir = make_image(rgb, W, H, W * 3), ib = make_image(bgr, W, H, W * 3);
        ib.order = FILM_ORDER_BGR;
        CHECK(develop_fresh(&params, &ir, false) == ESP_OK);
        CHECK(film_filter_develop(filter, &params, &ib) == ESP_OK);
        swap_rb(bgr, (size_t)W * H);
        if (memcmp(rgb, bgr, size) != 0) {
            fprintf(stderr, "bgr mismatch for film %d\n", film);
            s_failures++;
        }
    }
    film_image_t bad = make_image(rgb, W, H, W * 3);
    bad.order = (film_channel_order_t)7;
    film_develop_params_t params = { .film = FILM_ID_GOLD };
    CHECK(film_filter_develop(filter, &params, &bad) == ESP_ERR_INVALID_ARG);
    film_filter_delete(filter);
    free(rgb);
    free(bgr);
}

static double mean_level(const uint8_t *pixels, size_t bytes)
{
    double sum = 0.0;
    for (size_t i = 0; i < bytes; i++) {
        sum += pixels[i];
    }
    return sum / (double)bytes;
}

/* 曝光补偿单调：-2 < -1 < 0 < +1 < +2；超出范围按边界处理 */
static void test_exposure(void)
{
    enum { W = 96, H = 72 };
    static const film_id_t films[] = { FILM_ID_GOLD, FILM_ID_BW, FILM_ID_NIGHT, FILM_ID_PIXEL };
    static const float evs[] = { -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
    size_t size = W * H * 3;
    uint8_t *pixels = malloc(size), *clamped = malloc(size);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H };
    CHECK(pixels && clamped && film_filter_create(&config, &filter) == ESP_OK);
    film_image_t image = make_image(pixels, W, H, W * 3);

    for (size_t f = 0; f < sizeof(films) / sizeof(films[0]); f++) {
        double previous = -1.0;
        for (size_t e = 0; e < sizeof(evs) / sizeof(evs[0]); e++) {
            film_develop_params_t params = { .film = films[f], .seed = 2, .exposure_ev = evs[e] };
            fill_test_pattern(pixels, W, H, W * 3);
            CHECK(film_filter_develop(filter, &params, &image) == ESP_OK);
            double level = mean_level(pixels, size);
            if (level <= previous) {
                fprintf(stderr, "exposure not monotonic: film %d ev %.1f (%.2f <= %.2f)\n",
                        films[f], evs[e], level, previous);
                s_failures++;
            }
            previous = level;
        }
        /* +5 EV 与 +2 EV 结果相同 */
        film_develop_params_t over = { .film = films[f], .seed = 2, .exposure_ev = 5.0f };
        film_image_t ic = make_image(clamped, W, H, W * 3);
        fill_test_pattern(clamped, W, H, W * 3);
        CHECK(film_filter_develop(filter, &over, &ic) == ESP_OK);
        CHECK(memcmp(pixels, clamped, size) == 0);
    }
    film_filter_delete(filter);
    free(pixels);
    free(clamped);
}

/*
 * 快速红晕：首帧（没有可用遮罩）与完整算法一致；画面不变时第二帧用的遮罩就是本帧的，
 * 结果也应逐字节一致。换胶卷后自动退回完整算法。
 */
static void test_reuse_halation(void)
{
    enum { W = 200, H = 150 };
    size_t size = W * H * 3;
    uint8_t *fast = malloc(size), *full = malloc(size);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H, .parallel = true };
    CHECK(fast && full && film_filter_create(&config, &filter) == ESP_OK);
    film_image_t ifa = make_image(fast, W, H, W * 3), ifu = make_image(full, W, H, W * 3);
    film_develop_params_t params = {
        .film = FILM_ID_NIGHT, .seed = 6, .light_leak = true, .grain = true, .reuse_halation = true,
    };
    film_develop_params_t reference = params;
    reference.reuse_halation = false;

    fill_test_pattern(full, W, H, W * 3);
    CHECK(develop_fresh(&reference, &ifu, false) == ESP_OK);
    for (int frame = 0; frame < 3; frame++) {
        fill_test_pattern(fast, W, H, W * 3);
        CHECK(film_filter_develop(filter, &params, &ifa) == ESP_OK);
        if (memcmp(fast, full, size) != 0) {
            fprintf(stderr, "reuse halation mismatch at frame %d\n", frame);
            s_failures++;
        }
    }

    /* 中途换胶卷再换回来：首帧仍然走完整算法 */
    params.film = FILM_ID_GOLD;
    fill_test_pattern(fast, W, H, W * 3);
    CHECK(film_filter_develop(filter, &params, &ifa) == ESP_OK);
    params.film = FILM_ID_NIGHT;
    fill_test_pattern(fast, W, H, W * 3);
    CHECK(film_filter_develop(filter, &params, &ifa) == ESP_OK);
    CHECK(memcmp(fast, full, size) == 0);

    film_filter_delete(filter);
    free(fast);
    free(full);
}

/* 曝光变化也要正确失效缓存（含像素风插在中间） */
static void test_exposure_cache(void)
{
    static const struct {
        film_id_t film;
        float ev;
    } steps[] = {
        { FILM_ID_GOLD, 0.0f }, { FILM_ID_GOLD, 1.0f }, { FILM_ID_PIXEL, 1.0f }, { FILM_ID_GOLD, -0.5f },
        { FILM_ID_PIXEL, 0.0f }, { FILM_ID_GOLD, -0.5f }, { FILM_ID_NIGHT, 0.25f }, { FILM_ID_NIGHT, 0.0f },
    };
    enum { W = 64, H = 48 };
    uint8_t *reused = malloc(W * H * 3), *fresh = malloc(W * H * 3);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = { .max_width = W, .max_height = H };
    CHECK(reused && fresh && film_filter_create(&config, &filter) == ESP_OK);
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        film_develop_params_t params = { .film = steps[i].film, .seed = 3, .grain = true, .exposure_ev = steps[i].ev };
        film_image_t ir = make_image(reused, W, H, W * 3), ifr = make_image(fresh, W, H, W * 3);
        fill_test_pattern(reused, W, H, W * 3);
        fill_test_pattern(fresh, W, H, W * 3);
        CHECK(film_filter_develop(filter, &params, &ir) == ESP_OK);
        CHECK(develop_fresh(&params, &ifr, false) == ESP_OK);
        if (memcmp(reused, fresh, W * H * 3) != 0) {
            fprintf(stderr, "exposure cache mismatch at step %zu\n", i);
            s_failures++;
        }
    }
    film_filter_delete(filter);
    free(reused);
    free(fresh);
}

int main(void)
{
    test_bgr_order();
    test_exposure();
    test_reuse_halation();
    test_exposure_cache();
    test_arguments();
    test_keys();
    test_determinism();
    test_stride();
    test_film_properties();
    test_edge_sizes();
    test_parallel_matches_serial();
    test_table_cache();
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("film_filter_test: all checks passed\n");
    return 0;
}

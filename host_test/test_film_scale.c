/*
 * 滤镜的"相对尺寸"一致性：同一胶卷在 1440×1080、1536×1152、2048×1536、1536×2048 的成片上，
 * 平均亮度、暗角（角落/中心）、颗粒强度相同，颗粒占画面的比例随尺寸放大（相关长度 ∝ 长边）。
 * 用均匀灰图（暗角、颗粒是唯一的变化来源）统计。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_filter.h"

static int s_failures;
#define CHECK(cond, ...)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s  ", __FILE__, __LINE__, #cond);     \
            fprintf(stderr, __VA_ARGS__);                                                \
            fprintf(stderr, "\n");                                                       \
            s_failures++;                                                                \
        }                                                                                \
    } while (0)

typedef struct {
    int w, h;
} size_t2;

typedef struct {
    double mean;            /*!< 中心区域亮度均值 */
    double corner_ratio;    /*!< 四角小块均值 / 中心均值 */
    double grain_std;       /*!< 中心区域逐像素标准差 */
    double corr1;           /*!< 中心区域水平方向相邻像素的相关系数 */
} stats_t;

static double luma(const uint8_t *p)
{
    return 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
}

static double region_mean(const uint8_t *px, int w, int x0, int y0, int rw, int rh)
{
    double sum = 0;
    for (int y = y0; y < y0 + rh; ++y) {
        for (int x = x0; x < x0 + rw; ++x) {
            sum += luma(px + ((size_t)y * w + x) * 3);
        }
    }
    return sum / ((double)rw * rh);
}

static stats_t measure(film_filter_handle_t filter, film_id_t film, size_t2 size)
{
    const size_t bytes = (size_t)size.w * size.h * 3;
    uint8_t *px = malloc(bytes);
    memset(px, 128, bytes);
    const film_develop_params_t params = { .film = film, .seed = 11, .grain = true };
    const film_image_t image = { .pixels = px, .width = (uint16_t)size.w, .height = (uint16_t)size.h,
                                 .stride = (size_t)size.w * 3 };
    if (film_filter_develop(filter, &params, &image) != ESP_OK) {
        fprintf(stderr, "develop failed\n");
        exit(1);
    }
    stats_t st = { 0 };
    /* 中心 10%×10% 的区域：暗角在这里几乎不变，剩下的起伏就是颗粒 */
    const int rw = size.w / 10, rh = size.h / 10;
    const int x0 = (size.w - rw) / 2, y0 = (size.h - rh) / 2;
    st.mean = region_mean(px, size.w, x0, y0, rw, rh);
    double sq = 0, prod = 0;
    for (int y = y0; y < y0 + rh; ++y) {
        for (int x = x0; x + 1 < x0 + rw; ++x) {
            const double a = luma(px + ((size_t)y * size.w + x) * 3) - st.mean;
            const double b = luma(px + ((size_t)y * size.w + x + 1) * 3) - st.mean;
            sq += a * a;
            prod += a * b;
        }
    }
    st.grain_std = sqrt(sq / ((double)(rw - 1) * rh));
    st.corr1 = sq > 0 ? prod / sq : 0;
    const int cw = size.w / 20, ch = size.h / 20;
    st.corner_ratio = (region_mean(px, size.w, 0, 0, cw, ch) + region_mean(px, size.w, size.w - cw, 0, cw, ch) +
                       region_mean(px, size.w, 0, size.h - ch, cw, ch) +
                       region_mean(px, size.w, size.w - cw, size.h - ch, cw, ch)) / 4.0 / st.mean;
    free(px);
    return st;
}

int main(void)
{
    const film_filter_config_t config = { .max_width = 2112, .max_height = 2112, .parallel = true };
    film_filter_handle_t filter;
    if (film_filter_create(&config, &filter) != ESP_OK) {
        return 1;
    }
    const size_t2 sizes[] = { { 1440, 1080 }, { 1536, 1152 }, { 2048, 1536 }, { 1536, 2048 } };
    const film_id_t films[] = { FILM_ID_GOLD, FILM_ID_PORTRA, FILM_ID_NIGHT, FILM_ID_BW, FILM_ID_FADED };
    for (size_t f = 0; f < sizeof(films) / sizeof(films[0]); ++f) {
        stats_t st[4];
        for (size_t i = 0; i < 4; ++i) {
            st[i] = measure(filter, films[f], sizes[i]);
            printf("film %d %4dx%-4d mean %.2f corner %.3f grain std %.2f corr1 %.2f\n", (int)films[f], sizes[i].w,
                   sizes[i].h, st[i].mean, st[i].corner_ratio, st[i].grain_std, st[i].corr1);
        }
        for (size_t i = 1; i < 4; ++i) {
            CHECK(fabs(st[i].mean - st[0].mean) < 0.02 * st[0].mean, "film %d mean differs at size %zu", (int)films[f], i);
            /* 暗角占画面的比例相同：角落/中心之比各尺寸一致 */
            CHECK(fabs(st[i].corner_ratio - st[0].corner_ratio) < 0.03, "film %d vignette differs at size %zu",
                  (int)films[f], i);
            /* 颗粒的幅度相同 */
            CHECK(fabs(st[i].grain_std - st[0].grain_std) < 0.2 * st[0].grain_std + 0.05, "film %d grain std differs at %zu",
                  (int)films[f], i);
        }
        if (st[0].grain_std > 0.5) {
            /* 基准尺寸逐像素独立；更大的成片颗粒放大，相邻像素相关（2048 长边约 1.42 倍 → 相关系数 ≈ 0.3） */
            CHECK(fabs(st[0].corr1) < 0.06, "film %d baseline grain should be per-pixel, corr %.2f", (int)films[f],
                  st[0].corr1);
            CHECK(st[2].corr1 > 0.15, "film %d full-res grain not scaled, corr %.2f", (int)films[f], st[2].corr1);
            CHECK(st[3].corr1 > 0.15, "film %d portrait full-res grain not scaled, corr %.2f", (int)films[f], st[3].corr1);
        }
    }
    film_filter_delete(filter);
    printf(s_failures ? "film_scale: %d failure(s)\n" : "film_scale: all passed\n", s_failures);
    return s_failures ? 1 : 0;
}

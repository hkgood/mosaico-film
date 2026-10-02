/*
 * 暗房测试：几何（裁切、旋转、转正）、版式尺寸、条带边界，并输出样张供目视检查。
 *   film_darkroom_test <输出目录>
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "film_assets.h"
#include "film_darkroom.h"
#include "film_library.h"
#include "film_port.h"
#include "film_viewfinder.h"
#include "ppm_io.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/*
 * 合成"传感器"图：取景方向（竖 768×1024）四个象限四种颜色，再顺时针转 90° 存成横幅传感器图。
 * 与真机安装方向一致：传感器画面逆时针转 90° 才是取景方向。
 */
#define DISP_W 768
#define DISP_H 1024

static const uint8_t k_quad[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 255 } };

/** 传感器坐标 (x, y) 处应有的象限；disp_w×disp_h 为取景方向的完整画幅 */
static int sensor_quad(int x, int y, int disp_w, int disp_h)
{
    const int xd = y;
    const int yd = disp_h - 1 - x;
    const int crop_top = (disp_h - disp_w * 3 / 4) / 2;
    return (xd >= disp_w / 2 ? 1 : 0) + (yd >= crop_top + disp_w * 3 / 8 ? 2 : 0);
}

static void make_sensor(ppm_image_t *s)
{
    s->width = DISP_H;
    s->height = DISP_W;
    s->pixels = malloc((size_t)s->width * s->height * 3);
    CHECK(s->pixels);
    for (int y = 0; y < s->height; ++y) {
        for (int x = 0; x < s->width; ++x) {
            memcpy(s->pixels + ((size_t)y * s->width + x) * 3, k_quad[sensor_quad(x, y, DISP_W, DISP_H)], 3);
        }
    }
}

/* ---------------------------------------------------------------- 取景帧（UYVY → RGB565） */
#define VF_SENSOR_W 640
#define VF_SENSOR_H 480

static uint8_t to_u8(float v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
}

/** 与取景帧同规则的 UYVY 传感器图（BT.601 有限范围） */
static uint8_t *make_sensor_uyvy(void)
{
    uint8_t *buf = malloc((size_t)VF_SENSOR_W * VF_SENSOR_H * 2);
    CHECK(buf);
    for (int y = 0; y < VF_SENSOR_H; ++y) {
        for (int x = 0; x < VF_SENSOR_W; x += 2) {
            uint8_t *p = buf + ((size_t)y * VF_SENSOR_W + x) * 2;
            float yy[2], u = 0, v = 0;
            for (int k = 0; k < 2; ++k) {
                const uint8_t *c = k_quad[sensor_quad(x + k, y, VF_SENSOR_H, VF_SENSOR_W)];
                yy[k] = 16.0f + (65.738f * c[0] + 129.057f * c[1] + 25.064f * c[2]) / 256.0f;
                u += 0.5f * (128.0f + (-37.945f * c[0] - 74.494f * c[1] + 112.439f * c[2]) / 256.0f);
                v += 0.5f * (128.0f + (112.439f * c[0] - 94.154f * c[1] - 18.285f * c[2]) / 256.0f);
            }
            p[0] = to_u8(u);
            p[1] = to_u8(yy[0]);
            p[2] = to_u8(v);
            p[3] = to_u8(yy[1]);
        }
    }
    return buf;
}

static int quad_565(uint16_t px)
{
    const int rgb[3] = { (px >> 11) << 3, ((px >> 5) & 63) << 2, (px & 31) << 3 };
    for (int q = 0; q < 4; ++q) {
        if (abs(rgb[0] - k_quad[q][0]) < 40 && abs(rgb[1] - k_quad[q][1]) < 40 && abs(rgb[2] - k_quad[q][2]) < 40) {
            return q;
        }
    }
    return -1;
}

/** 取景帧四角与中心两侧的象限应与成片（ROT_0）相同：左上 0、右上 1、左下 2、右下 3 */
static void test_viewfinder(void)
{
    uint8_t *uyvy = make_sensor_uyvy();
    uint16_t *out = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    CHECK(out);
    const film_viewfinder_source_t src = { .uyvy = uyvy, .width = VF_SENSOR_W, .height = VF_SENSOR_H };
    for (int instant = 0; instant <= 1; ++instant) {
        uint16_t w, h;
        film_viewfinder_size(instant, &w, &h);
        CHECK(film_viewfinder_convert(&src, instant, out) == ESP_OK);
        const int ix = w / 10, iy = h / 10;
        CHECK(quad_565(out[iy * w + ix]) == 0);
        CHECK(quad_565(out[iy * w + w - 1 - ix]) == 1);
        CHECK(quad_565(out[(h - 1 - iy) * w + ix]) == 2);
        CHECK(quad_565(out[(h - 1 - iy) * w + w - 1 - ix]) == 3);
        /* 象限分界落在正中（裁切居中） */
        CHECK(quad_565(out[(h / 2 - 4) * w + w / 2 - 4]) == 0);
        CHECK(quad_565(out[(h / 2 + 4) * w + w / 2 + 4]) == 3);
    }
    /* 竖幅传感器不支持 */
    const film_viewfinder_source_t tall = { .uyvy = uyvy, .width = VF_SENSOR_H, .height = VF_SENSOR_W };
    CHECK(film_viewfinder_convert(&tall, false, out) == ESP_ERR_INVALID_SIZE);
    free(out);
    free(uyvy);
}

typedef struct {
    const ppm_image_t *image;
    int row;
    int strip;
    uint8_t *reuse;     /*!< 非空：像真机解码器一样，每条带都拷进同一块缓冲再交出去 */
} src_t;

static esp_err_t src_read(void *ctx, const uint8_t **rows, size_t *stride, int *count)
{
    src_t *s = ctx;
    const int left = s->image->height - s->row;
    *count = left < s->strip ? left : s->strip;
    *stride = (size_t)s->image->width * 3;
    const uint8_t *from = s->image->pixels + (size_t)s->row * *stride;
    if (s->reuse) {
        memcpy(s->reuse, from, (size_t)*count * *stride);
        from = s->reuse;
    }
    *rows = from;
    s->row += *count;
    return ESP_OK;
}

static void *test_alloc(size_t n, void *ctx)
{
    (void)ctx;
    return malloc(n);
}

static void test_free(void *p, void *ctx)
{
    (void)ctx;
    free(p);
}

static int quad_at(const film_darkroom_image_t *img, int x, int y)
{
    const uint8_t *p = img->pixels + (size_t)y * img->stride + (size_t)x * 3;
    for (int q = 0; q < 4; ++q) {
        if (abs(p[0] - k_quad[q][0]) < 40 && abs(p[1] - k_quad[q][1]) < 40 && abs(p[2] - k_quad[q][2]) < 40) {
            return q;
        }
    }
    return -1;
}

static film_darkroom_image_t develop_from(film_darkroom_handle_t dr, const ppm_image_t *sensor,
                                          film_darkroom_job_t job, int strip, uint8_t *reuse)
{
    src_t s = { .image = sensor, .strip = strip, .reuse = reuse };
    const film_darkroom_source_t source = {
        .width = (uint16_t)sensor->width, .height = (uint16_t)sensor->height, .ctx = &s, .read = src_read,
    };
    film_darkroom_image_t img;
    CHECK(film_darkroom_develop(dr, &job, &source, &img) == ESP_OK);
    return img;
}

static film_darkroom_image_t develop(film_darkroom_handle_t dr, const ppm_image_t *sensor, film_darkroom_job_t job,
                                     int strip)
{
    return develop_from(dr, sensor, job, strip, NULL);
}

/** 数据源复用条带缓冲时，跨条带插值仍须用上一条带的真实末行（曾因此每 16 行出现一道竖纹） */
static void test_reused_strip(film_darkroom_handle_t dr, const ppm_image_t *sample)
{
    enum { STRIP = 16 };
    uint8_t *reuse = malloc((size_t)STRIP * sample->width * 3);
    CHECK(reuse);
    for (int instant = 0; instant < 2; ++instant) {
        const film_darkroom_job_t job = { .film = FILM_ID_COUNT, .rot = GFX_ROT_90, .instant = instant != 0 };
        film_darkroom_image_t direct = develop_from(dr, sample, job, STRIP, NULL);
        film_darkroom_image_t copied = develop_from(dr, sample, job, STRIP, reuse);
        CHECK(direct.width == copied.width && direct.height == copied.height);
        for (int y = 0; y < direct.height; ++y) {
            CHECK(memcmp(direct.pixels + (size_t)y * direct.stride, copied.pixels + (size_t)y * copied.stride,
                         (size_t)direct.width * 3) == 0);
        }
        film_darkroom_release(dr, &copied);
        film_darkroom_release(dr, &direct);
    }
    free(reuse);
}

static bool near_ratio(float value, float expect, float tolerance)
{
    return value > expect * (1.0f - tolerance) && value < expect * (1.0f + tolerance);
}

/** 校正编码：往返、截断、无效统计 */
static void test_balance_code(void)
{
    float gain[3];
    film_balance_gains(FILM_BALANCE_NONE, gain);
    CHECK(gain[0] == 1.0f && gain[1] == 1.0f && gain[2] == 1.0f);

    const film_tone_stats_t from = { .mean = { 0.05f, 0.1f, 0.04f }, .valid = true };
    const film_tone_stats_t to = { .mean = { 0.2f, 0.2f, 0.2f }, .valid = true };
    film_balance_gains(film_balance_match(&from, NULL, &to), gain);
    CHECK(near_ratio(gain[0], 4.0f, 0.03f) && near_ratio(gain[1], 2.0f, 0.03f) && near_ratio(gain[2], 5.0f, 0.03f));

    /* 亮度超出 +2.94 档（11 × 1/4 + 3 × 1/16）时截断 */
    const film_tone_stats_t dark = { .mean = { 0.001f, 0.001f, 0.001f }, .valid = true };
    film_balance_gains(film_balance_match(&dark, NULL, &to), gain);
    CHECK(near_ratio(gain[1], 7.66f, 0.02f));

    /* 负的亮度：粗字段向下取整，细分位补回（-0.2 档 → -3/16 档） */
    const film_tone_stats_t bright = { .mean = { 0.23f, 0.23f, 0.23f }, .valid = true };
    const film_balance_t dim = film_balance_match(&bright, NULL, &to);
    film_balance_gains(dim, gain);
    CHECK(near_ratio(gain[1], 0.878f, 0.01f));

    /* 存进元数据（低 16 位 + flags 细分位）再拼回，编码不变 */
    film_photo_t meta = { .balance = (uint16_t)dim,
                          .flags = (uint8_t)(FILM_PHOTO_DATE | (((dim >> FILM_BALANCE_FINE_SHIFT) &
                                                                FILM_BALANCE_FINE_MASK) << FILM_PHOTO_BALANCE_FINE_SHIFT)) };
    CHECK((dim >> FILM_BALANCE_FINE_SHIFT) != 0);
    CHECK(film_photo_balance(&meta) == dim);

    film_tone_stats_t invalid = to;
    invalid.valid = false;
    CHECK(film_balance_match(&invalid, NULL, &to) == FILM_BALANCE_NONE);
    CHECK(film_balance_match(&from, NULL, NULL) == FILM_BALANCE_NONE);
}

/**
 * 原片对齐：小样（屏幕尺寸、颜色正常）给出参考均值，
 * 偏绿偏暗的全尺寸原片冲洗后应回到参考均值；按存档编码重新冲洗得到同样的结果。
 */
static void test_balance_match(film_darkroom_handle_t dr, const ppm_image_t *sample)
{
    film_tone_stats_t ref;
    film_darkroom_job_t job = { .film = FILM_ID_COUNT, .size = FILM_DARKROOM_SCREEN, .ret_stats = &ref };
    film_darkroom_image_t proof = develop(dr, sample, job, 16);
    film_darkroom_release(dr, &proof);
    CHECK(ref.valid);

    /* 模拟拍照模式：线性光 R、B ×0.3，G ×0.45（偏绿，暗约 1.2 档） */
    static const float k_tint[3] = { 0.3f, 0.45f, 0.3f };
    ppm_image_t tinted = *sample;
    const size_t bytes = (size_t)sample->width * sample->height * 3;
    tinted.pixels = malloc(bytes);
    CHECK(tinted.pixels);
    for (size_t i = 0; i < bytes; ++i) {
        tinted.pixels[i] = (uint8_t)lroundf((float)sample->pixels[i] * sqrtf(k_tint[i % 3]));
    }

    film_tone_stats_t before;
    film_balance_t code = FILM_BALANCE_NONE;
    job = (film_darkroom_job_t) { .film = FILM_ID_COUNT, .match = &ref, .ret_stats = &before, .ret_balance = &code };
    film_darkroom_image_t matched = develop(dr, &tinted, job, 16);
    CHECK(before.valid && code != FILM_BALANCE_NONE);
    for (int c = 0; c < 3; ++c) {
        CHECK(near_ratio(before.mean[c], ref.mean[c] * k_tint[c], 0.1f));
    }
    film_tone_stats_t after;
    film_balance_measure(matched.pixels, matched.stride, matched.width, matched.height, &after, NULL);
    /* 精确求解后只剩量化误差：亮度每格 1/16 档（最多差 ≈2%），色度每格 1/16 档 */
    for (int c = 0; c < 3; ++c) {
        CHECK(near_ratio(after.mean[c], ref.mean[c], 0.04f));
    }
    /* 绿色偏差基本消除：G/R、G/B 与参考相差不到 5% */
    CHECK(near_ratio(after.mean[1] / after.mean[0], ref.mean[1] / ref.mean[0], 0.05f));
    CHECK(near_ratio(after.mean[1] / after.mean[2], ref.mean[1] / ref.mean[2], 0.05f));

    job = (film_darkroom_job_t) { .film = FILM_ID_COUNT, .balance = code };
    film_darkroom_image_t again = develop(dr, &tinted, job, 16);
    CHECK(again.size == matched.size && memcmp(again.pixels, matched.pixels, matched.size) == 0);
    film_darkroom_release(dr, &again);
    film_darkroom_release(dr, &matched);
    free(tinted.pixels);
}

static void save(const char *dir, const char *name, const film_darkroom_image_t *img)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    const ppm_image_t out = { .pixels = img->pixels, .width = img->width, .height = img->height };
    CHECK(ppm_write(path, &out));
}

static esp_err_t bind_assets(void)
{
    FILE *f = fopen(FILM_TEST_ASSETS, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    CHECK(data && fread(data, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return film_assets_bind(data, (size_t)n);   /* 测试进程内一直有效 */
}

int main(int argc, char **argv)
{
    const char *out_dir = argc > 1 ? argv[1] : "darkroom_out";
    mkdir(out_dir, 0755);
    CHECK(bind_assets() == ESP_OK);

    film_filter_handle_t filter;
    const film_filter_config_t fc = { .max_width = 1440, .max_height = 1440, .parallel = true };
    CHECK(film_filter_create(&fc, &filter) == ESP_OK);
    film_darkroom_handle_t dr;
    const film_darkroom_config_t dc = { .filter = filter, .alloc = test_alloc, .free = test_free };
    CHECK(film_darkroom_create(&dc, &dr) == ESP_OK);

    /* 1. 几何：未冲洗的原片，四个角的象限颜色符合裁切 + 转正规则 */
    ppm_image_t synth;
    make_sensor(&synth);
    /* 各握持方向下，转正图像左上、右上、左下、右下四角应出现的象限 */
    static const int expect[4][4] = {
        { 0, 1, 2, 3 },     /* ROT_0：与取景方向相同 */
        { 1, 3, 0, 2 },     /* ROT_90：屏幕坐标 x = W-1-v, y = u */
        { 3, 2, 1, 0 },     /* ROT_180 */
        { 2, 0, 3, 1 },     /* ROT_270：x = v, y = H-1-u */
    };
    for (int rot = 0; rot < 4; ++rot) {
        const film_darkroom_job_t job = { .film = FILM_ID_COUNT, .rot = (gfx_rot_t)rot };
        film_darkroom_image_t img = develop(dr, &synth, job, 13);
        const bool swap = rot == 1 || rot == 3;
        CHECK(img.width == (swap ? FILM_DARKROOM_M6_H : FILM_DARKROOM_M6_W));
        CHECK(img.height == (swap ? FILM_DARKROOM_M6_W : FILM_DARKROOM_M6_H));
        const int ix = img.width / 10, iy = img.height / 10;
        CHECK(quad_at(&img, ix, iy) == expect[rot][0]);
        CHECK(quad_at(&img, img.width - 1 - ix, iy) == expect[rot][1]);
        CHECK(quad_at(&img, ix, img.height - 1 - iy) == expect[rot][2]);
        CHECK(quad_at(&img, img.width - 1 - ix, img.height - 1 - iy) == expect[rot][3]);
        /* 象限分界应落在图像正中（裁切居中） */
        CHECK(quad_at(&img, img.width / 2 - 8, img.height / 2 - 8) == expect[rot][0]);
        CHECK(quad_at(&img, img.width / 2 + 8, img.height / 2 + 8) == expect[rot][3]);
        film_darkroom_release(dr, &img);
    }
    test_viewfinder();

    /* 2. 宝丽来：相纸尺寸、图像位置、相纸区域不被覆盖 */
    {
        const film_darkroom_job_t job = { .film = FILM_ID_COUNT, .instant = true };
        film_darkroom_image_t img = develop(dr, &synth, job, 8);
        CHECK(img.width == FILM_DARKROOM_PRINT_W && img.height == FILM_DARKROOM_PRINT_H);
        CHECK(quad_at(&img, FILM_DARKROOM_PRINT_SIDE + 20, FILM_DARKROOM_PRINT_TOP + 20) == 0);
        CHECK(quad_at(&img, FILM_DARKROOM_PRINT_SIDE + FILM_DARKROOM_PRINT_IMAGE - 20,
                      FILM_DARKROOM_PRINT_TOP + FILM_DARKROOM_PRINT_IMAGE - 20) == 3);
        CHECK(quad_at(&img, 10, img.height - 60) == 3);   /* 下边是接近白色的相纸 */
        film_darkroom_release(dr, &img);
    }

    /* 3. 屏幕尺寸与取景尺寸 */
    {
        int w, h;
        film_darkroom_job_t job = { .instant = true, .size = FILM_DARKROOM_SCREEN };
        film_darkroom_output_size(&job, &w, &h);
        CHECK(w == 296 && h == 360);
        job = (film_darkroom_job_t) { .size = FILM_DARKROOM_VIEWFINDER, .rot = GFX_ROT_90 };
        film_darkroom_output_size(&job, &w, &h);
        CHECK(w == 480 && h == 360);
        job.instant = true;
        film_darkroom_output_size(&job, &w, &h);
        CHECK(w == 360 && h == 360);
    }

    /* 4. 样张：真实样片 + 胶卷 + 日期戳，供目视检查 */
    ppm_image_t sample;
    CHECK(ppm_read(FILM_TEST_SENSOR, &sample));
    test_reused_strip(dr, &sample);
    test_balance_code();
    test_balance_match(dr, &sample);
    struct {
        const char *name;
        film_darkroom_job_t job;
    } shots[] = {
        { "m6_gold_rot0", { .film = FILM_ID_GOLD, .seed = 7, .stamp = "'26 09 30" } },
        { "m6_night_rot90", { .film = FILM_ID_NIGHT, .seed = 7, .rot = GFX_ROT_90, .stamp = "'26 09 30" } },
        { "sx_verde", { .film = FILM_ID_GREEN, .seed = 3, .instant = true, .stamp = "'26 09 30" } },
        { "sx_screen_pixel", { .film = FILM_ID_PIXEL, .seed = 3, .instant = true, .size = FILM_DARKROOM_SCREEN } },
        { "vf_cross", { .film = FILM_ID_CROSS, .seed = 3, .size = FILM_DARKROOM_VIEWFINDER } },
    };
    for (size_t i = 0; i < sizeof(shots) / sizeof(shots[0]); ++i) {
        film_darkroom_image_t img = develop(dr, &sample, shots[i].job, 16);
        save(out_dir, shots[i].name, &img);
        /* 缩略图路径也走一遍 */
        int tw, th;
        film_darkroom_fit(img.width, img.height, 160, 160, &tw, &th);
        uint16_t *thumb = malloc(sizeof(uint16_t) * (size_t)tw * th);
        uint32_t *scratch = malloc(sizeof(uint32_t) * 3 * (size_t)tw);
        CHECK(thumb && scratch);
        film_darkroom_downscale_565(&img, thumb, tw, th, scratch);
        free(thumb);
        free(scratch);
        film_darkroom_release(dr, &img);
    }

    free(sample.pixels);
    free(synth.pixels);
    film_darkroom_delete(dr);
    film_filter_delete(filter);
    printf("film_darkroom_test: OK (samples in %s)\n", out_dir);
    return 0;
}

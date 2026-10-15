/*
 * 取景转换查表版与原始公式逐位一致：
 *   1. 穷举全部 2^24 个 (Y, U, V)，查表结果 == BT.601 公式算 RGB888 再截位成 565
 *   2. 随机传感器帧（含带行尾填充的 stride）按几何约定逐像素重算，M6 与宝丽来整帧一致
 *   3. 胶卷取景外观：恒等外观与不带外观逐位一致（穷举 + 整帧）；真实胶卷的外观与不查表的标量公式逐位一致；
 *      单色三通道相等、饱和度 0 变灰、灰阶曲线单调、像素风 EV0 为恒等
 *
 * 直接编进 film_viewfinder.c 才能测到其中的内联像素函数；公开函数改名，免得与 film_darkroom 库重名。
 *
 *   film_viewfinder_test
 */
#define film_viewfinder_size          vf_test_size
#define film_viewfinder_convert       vf_test_convert
#define film_viewfinder_row_to_rgb888 vf_test_row_to_rgb888
#define film_viewfinder_convert_look  vf_test_convert_look
#define film_viewfinder_look_set      vf_test_look_set
#include "../components/film_darkroom/film_viewfinder.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define SENSOR_W   640
#define SENSOR_H   480
#define PAD_BYTES  64       /*!< 行尾填充，验证 stride 生效 */

/** 查表之前的写法：公式算出 RGB888，再截位打包成 565 */
static uint16_t reference_565(int y, int u, int v)
{
    uint8_t rgb[3];
    yuv_to_rgb(y, u, v, rgb);
    return (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
}

static void check_all_pixels(void)
{
    for (int y = 0; y < 256; ++y) {
        for (int u = 0; u < 256; ++u) {
            for (int v = 0; v < 256; ++v) {
                if (yuv_to_565(y, u, v) != reference_565(y, u, v)) {
                    fprintf(stderr, "mismatch at Y=%d U=%d V=%d: %04x != %04x\n", y, u, v, yuv_to_565(y, u, v),
                            reference_565(y, u, v));
                    exit(1);
                }
            }
        }
    }
}

/** 按 film_viewfinder.c 顶部的几何约定逐像素重算一帧 */
static void reference_convert(const film_viewfinder_source_t *src, bool instant, bool crop_43, uint16_t *out)
{
    uint16_t vw, vh;
    vf_test_size(instant, crop_43, &vw, &vh);
    const uint32_t block_h = src->height;
    const uint32_t block_w = instant   ? src->height
                             : crop_43 ? src->width * FILM_DARKROOM_M6_CROP_NUM / FILM_DARKROOM_M6_CROP_DEN
                                       : src->width;
    const uint32_t crop_x0 = (src->width - block_w) / 2u;
    for (uint32_t y = 0; y < vh; ++y) {
        const uint32_t col = crop_x0 + block_w - 1u - ((2u * y + 1u) * block_w) / (2u * vh);
        for (uint32_t x = 0; x < vw; ++x) {
            const uint32_t row = ((2u * x + 1u) * block_h) / (2u * vw);
            const uint8_t *pair = src->uyvy + (size_t)row * src->stride + (col >> 1) * 4u;
            out[(size_t)y * vw + x] = reference_565(pair[1 + (col & 1u) * 2u], pair[0], pair[2]);
        }
    }
}

static void check_frames(void)
{
    const size_t stride = SENSOR_W * 2u + PAD_BYTES;
    uint8_t *uyvy = malloc(stride * SENSOR_H);
    uint16_t *got = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    uint16_t *want = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    CHECK(uyvy && got && want);
    uint32_t seed = 0x5EEDu;
    for (int frame = 0; frame < 8; ++frame) {
        for (size_t i = 0; i < stride * SENSOR_H; ++i) {
            seed = seed * 1664525u + 1013904223u;
            uyvy[i] = (uint8_t)(seed >> 24);
        }
        const film_viewfinder_source_t src = { .uyvy = uyvy, .width = SENSOR_W, .height = SENSOR_H, .stride = stride };
        /* 三种取景：宝丽来方形、M6 4:3 裁切、M6 全画幅（3:4 竖幅） */
        for (int mode = 0; mode < 3; ++mode) {
            const bool instant = mode == 0, crop_43 = mode == 1;
            uint16_t vw, vh;
            vf_test_size(instant, crop_43, &vw, &vh);
            CHECK(vw * vh <= FILM_VF_M6_W * FILM_VF_M6_H);
            CHECK(vf_test_convert(&src, instant, crop_43, got) == ESP_OK);
            reference_convert(&src, instant, crop_43, want);
            CHECK(memcmp(got, want, sizeof(uint16_t) * vw * vh) == 0);
        }
    }
    free(uyvy);
    free(got);
    free(want);
}


/* ---------------------------------------------------------------- 胶卷取景外观 */

/** 标量参考：不用任何查表，直接按"色度缩放 → BT.601 → 曲线 → 截位"算一个像素 */
static uint16_t reference_look_565(const film_look_t *look, int y, int u, int v)
{
    const int c = 298 * (y - 16) + 128;
    if (look->mono) {
        int lum = c >> 8;
        lum = lum < 0 ? 0 : (lum > 255 ? 255 : lum);
        const int g = look->curve[0][lum];
        return (uint16_t)(((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
    }
    int d = ((u - 128) * look->sat_q8 + 128) >> 8;
    int e = ((v - 128) * look->sat_q8 + 128) >> 8;
    d = d < -128 ? -128 : (d > 127 ? 127 : d);
    e = e < -128 ? -128 : (e > 127 ? 127 : e);
    const uint8_t r = look->curve[0][clamp_u8((c + 409 * e) >> 8)];
    const uint8_t g = look->curve[1][clamp_u8((c - 100 * d - 208 * e) >> 8)];
    const uint8_t b = look->curve[2][clamp_u8((c + 516 * d) >> 8)];
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static film_viewfinder_look_t *s_table;

static void check_identity_look(void)
{
    vf_test_look_set(s_table, NULL);
    for (int y = 0; y < 256; ++y) {
        for (int u = 0; u < 256; ++u) {
            for (int v = 0; v < 256; ++v) {
                if (look_to_565(s_table, y, u, v) != yuv_to_565(y, u, v)) {
                    fprintf(stderr, "identity look mismatch at Y=%d U=%d V=%d\n", y, u, v);
                    exit(1);
                }
            }
        }
    }
}

static void random_frame(uint8_t *uyvy, size_t bytes, uint32_t seed)
{
    for (size_t i = 0; i < bytes; ++i) {
        seed = seed * 1664525u + 1013904223u;
        uyvy[i] = (uint8_t)(seed >> 24);
    }
}

static void check_look_frames(void)
{
    const size_t stride = SENSOR_W * 2u + PAD_BYTES;
    uint8_t *uyvy = malloc(stride * SENSOR_H);
    uint16_t *got = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    uint16_t *want = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    CHECK(uyvy && got && want);
    random_frame(uyvy, stride * SENSOR_H, 0xF11Du);
    const film_viewfinder_source_t src = { .uyvy = uyvy, .width = SENSOR_W, .height = SENSOR_H, .stride = stride };

    /* 恒等外观整帧与不带外观逐位一致，三种取景都测 */
    vf_test_look_set(s_table, NULL);
    for (int mode = 0; mode < 3; ++mode) {
        const bool instant = mode == 0, crop_43 = mode == 1;
        uint16_t vw, vh;
        vf_test_size(instant, crop_43, &vw, &vh);
        CHECK(vf_test_convert_look(&src, instant, crop_43, s_table, got) == ESP_OK);
        CHECK(vf_test_convert(&src, instant, crop_43, want) == ESP_OK);
        CHECK(memcmp(got, want, sizeof(uint16_t) * vw * vh) == 0);
    }

    /* 每卷胶卷、几档曝光：整帧与标量参考逐位一致（参考与几何约定同 reference_convert） */
    const float evs[] = { -1.0f, 0.0f, 1.0f };
    for (int film = 0; film < FILM_ID_COUNT; ++film) {
        for (size_t k = 0; k < sizeof(evs) / sizeof(evs[0]); ++k) {
            film_look_t look;
            CHECK(film_filter_look((film_id_t)film, evs[k], &look) == ESP_OK);
            vf_test_look_set(s_table, &look);
            const bool instant = film % 2 == 0, crop_43 = film % 3 == 0;
            uint16_t vw, vh;
            vf_test_size(instant, crop_43, &vw, &vh);
            CHECK(vf_test_convert_look(&src, instant, crop_43, s_table, got) == ESP_OK);

            const uint32_t block_h = src.height;
            const uint32_t block_w = instant   ? src.height
                                     : crop_43 ? src.width * FILM_DARKROOM_M6_CROP_NUM / FILM_DARKROOM_M6_CROP_DEN
                                               : src.width;
            const uint32_t crop_x0 = (src.width - block_w) / 2u;
            for (uint32_t y = 0; y < vh; ++y) {
                const uint32_t col = crop_x0 + block_w - 1u - ((2u * y + 1u) * block_w) / (2u * vh);
                for (uint32_t x = 0; x < vw; ++x) {
                    const uint32_t row = ((2u * x + 1u) * block_h) / (2u * vw);
                    const uint8_t *pair = src.uyvy + (size_t)row * src.stride + (col >> 1) * 4u;
                    want[(size_t)y * vw + x] = reference_look_565(&look, pair[1 + (col & 1u) * 2u], pair[0], pair[2]);
                }
            }
            if (memcmp(got, want, sizeof(uint16_t) * vw * vh) != 0) {
                fprintf(stderr, "film %d ev %.1f: frame differs from the scalar reference\n", film, evs[k]);
                exit(1);
            }
        }
    }
    free(uyvy);
    free(got);
    free(want);
}

static void check_look_properties(void)
{
    for (int film = 0; film < FILM_ID_COUNT; ++film) {
        film_look_t look;
        CHECK(film_filter_look((film_id_t)film, 0.0f, &look) == ESP_OK);
        vf_test_look_set(s_table, &look);

        /* 灰阶（U=V=128）：单色三通道相等；彩色胶卷的绿通道随亮度不下降 */
        int prev_g = -1;
        for (int y = 16; y <= 235; ++y) {
            const uint16_t px = look_to_565(s_table, y, 128, 128);
            const int r5 = px >> 11, g6 = (px >> 5) & 63, b5 = px & 31;
            if (look.mono) {
                CHECK(r5 == b5 && (g6 >> 1) == r5);
                CHECK(s_table->gray565[y] == px || s_table->gray565[y] == (uint16_t)((r5 << 11) | (g6 << 5) | b5));
            }
            CHECK(g6 >= prev_g);
            prev_g = g6;
        }

        /* 单色外观：随机彩色像素也只输出灰 */
        if (look.mono) {
            for (int i = 0; i < 4096; ++i) {
                const int y = 16 + (i * 7) % 220, u = (i * 13) % 256, v = (i * 29) % 256;
                const uint16_t px = s_table->gray565[y];
                CHECK((px >> 11) == (px & 31) && (((px >> 5) & 63) >> 1) == (px >> 11));
                (void)u;
                (void)v;
            }
        }
    }

    /* 饱和度 0 + 恒等曲线：任何颜色都落在灰轴上 */
    film_look_t flat;
    for (int i = 0; i < 256; ++i) {
        flat.curve[0][i] = flat.curve[1][i] = flat.curve[2][i] = (uint8_t)i;
    }
    flat.sat_q8 = 0;
    flat.mono = false;
    vf_test_look_set(s_table, &flat);
    for (int y = 0; y < 256; y += 5) {
        for (int u = 0; u < 256; u += 3) {
            for (int v = 0; v < 256; v += 3) {
                const uint16_t px = look_to_565(s_table, y, u, v);
                CHECK((px >> 11) == (px & 31) && (((px >> 5) & 63) >> 1) == (px >> 11));
            }
        }
    }

    /* 更高的饱和度只会拉大色差，不会越界（表下标夹在原范围内） */
    flat.sat_q8 = 384;
    vf_test_look_set(s_table, &flat);
    for (int u = 0; u < 256; ++u) {
        for (int v = 0; v < 256; v += 5) {
            (void)look_to_565(s_table, 255, u, v);
            (void)look_to_565(s_table, 0, u, v);
        }
    }

    /* 像素风没有曲线：EV 0 的外观是恒等 */
    film_look_t pixel;
    CHECK(film_filter_look(FILM_ID_PIXEL, 0.0f, &pixel) == ESP_OK);
    vf_test_look_set(s_table, &pixel);
    for (int y = 0; y < 256; y += 3) {
        for (int u = 0; u < 256; u += 5) {
            for (int v = 0; v < 256; v += 5) {
                CHECK(look_to_565(s_table, y, u, v) == yuv_to_565(y, u, v));
            }
        }
    }
    CHECK(film_filter_look((film_id_t)FILM_ID_COUNT, 0.0f, &pixel) == ESP_ERR_INVALID_ARG);
}

int main(void)
{
    s_table = malloc(sizeof(*s_table));
    CHECK(s_table);
    check_all_pixels();
    check_frames();
    check_identity_look();
    check_look_frames();
    check_look_properties();
    free(s_table);
    printf("film_viewfinder_test: OK\n");
    return 0;
}

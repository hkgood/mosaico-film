/*
 * 取景转换查表版与原始公式逐位一致：
 *   1. 穷举全部 2^24 个 (Y, U, V)，查表结果 == BT.601 公式算 RGB888 再截位成 565
 *   2. 随机传感器帧（含带行尾填充的 stride）按几何约定逐像素重算，M6 与宝丽来整帧一致
 *
 * 直接编进 film_viewfinder.c 才能测到其中的内联像素函数；公开函数改名，免得与 film_darkroom 库重名。
 *
 *   film_viewfinder_test
 */
#define film_viewfinder_size          vf_test_size
#define film_viewfinder_convert       vf_test_convert
#define film_viewfinder_row_to_rgb888 vf_test_row_to_rgb888
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
static void reference_convert(const film_viewfinder_source_t *src, bool instant, uint16_t *out)
{
    uint16_t vw, vh;
    vf_test_size(instant, &vw, &vh);
    const uint32_t block_h = src->height;
    const uint32_t block_w = instant ? src->height
                                     : src->width * FILM_DARKROOM_M6_CROP_NUM / FILM_DARKROOM_M6_CROP_DEN;
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
        for (int instant = 0; instant <= 1; ++instant) {
            uint16_t vw, vh;
            vf_test_size(instant, &vw, &vh);
            CHECK(vf_test_convert(&src, instant, got) == ESP_OK);
            reference_convert(&src, instant, want);
            CHECK(memcmp(got, want, sizeof(uint16_t) * vw * vh) == 0);
        }
    }
    free(uyvy);
    free(got);
    free(want);
}

int main(void)
{
    check_all_pixels();
    check_frames();
    printf("film_viewfinder_test: OK\n");
    return 0;
}

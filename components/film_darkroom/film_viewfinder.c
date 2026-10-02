/*
 * 取景帧转换。几何与 film_darkroom.c 相同：
 *
 *   取景方向的第 x 列 ← 传感器第 row(x) 行（从上往下）
 *   取景方向的第 y 行 ← 传感器第 col(y) 列（从裁切块右边往左）
 *
 * 即传感器画面逆时针转 90°。取景帧按行写入时，同一行相邻像素来自不同的传感器行，
 * 直接逐像素读会在 PSRAM 上跳行；所以按 TILE_ROWS 行一组处理：
 * 组内每一列只读传感器一行里相邻的几十个字节，写入的几行取景缓冲也都留在缓存里。
 */
#include "film_viewfinder.h"

#include "film_darkroom.h"
#include "film_port.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
/* 查表放内部 RAM：取景时整帧 PSRAM 数据流过缓存，放 flash 会被反复挤出 */
#define LUT_ATTR DRAM_ATTR
#else
#define LUT_ATTR
#endif

#define TILE_ROWS 16    /*!< 每组取景行数；列偏移表 2×16×2 字节，放在栈上也只有 64 字节 */

/*
 * RGB565 查表：BT.601 每个分量 (c + 色度项) >> 8 都落在 [-277, 534]（B 分量最宽），
 * 加上 CLAMP_OFS 作下标，一次查表同时完成截断到 0..255、截位、移到 565 里的位置，
 * 省掉逐像素的比较分支。表在编译期生成（只读、线程安全），约 5 KB。
 */
#define CLAMP_OFS 288
#define CLAMP_LEN 832

#define REP4(f, i)   f(i), f((i) + 1), f((i) + 2), f((i) + 3)
#define REP16(f, i)  REP4(f, i), REP4(f, (i) + 4), REP4(f, (i) + 8), REP4(f, (i) + 12)
#define REP64(f, i)  REP16(f, i), REP16(f, (i) + 16), REP16(f, (i) + 32), REP16(f, (i) + 48)
#define REP256(f, i) REP64(f, i), REP64(f, (i) + 64), REP64(f, (i) + 128), REP64(f, (i) + 192)
#define REP832(f)    REP256(f, 0), REP256(f, 256), REP256(f, 512), REP64(f, 768)

#define CLAMPED(i) ((i) < CLAMP_OFS ? 0 : (i) - CLAMP_OFS > 255 ? 255 : (i) - CLAMP_OFS)
#define R565(i)    (uint16_t)((CLAMPED(i) >> 3) << 11)
#define G565(i)    (uint16_t)((CLAMPED(i) >> 2) << 5)
#define B565(i)    (uint16_t)(CLAMPED(i) >> 3)

static LUT_ATTR const uint16_t k_r565[CLAMP_LEN] = { REP832(R565) };
static LUT_ATTR const uint16_t k_g565[CLAMP_LEN] = { REP832(G565) };
static LUT_ATTR const uint16_t k_b565[CLAMP_LEN] = { REP832(B565) };

void film_viewfinder_size(bool instant, uint16_t *ret_w, uint16_t *ret_h)
{
    *ret_w = instant ? FILM_VF_SX_W : FILM_VF_M6_W;
    *ret_h = instant ? FILM_VF_SX_H : FILM_VF_M6_H;
}

static inline uint8_t clamp_u8(int v)
{
    return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

/** BT.601 有限范围 YUV → RGB888 */
static inline void yuv_to_rgb(int y, int u, int v, uint8_t *rgb)
{
    const int c = 298 * (y - 16) + 128;
    const int d = u - 128;
    const int e = v - 128;
    rgb[0] = clamp_u8((c + 409 * e) >> 8);
    rgb[1] = clamp_u8((c - 100 * d - 208 * e) >> 8);
    rgb[2] = clamp_u8((c + 516 * d) >> 8);
}

/** BT.601 有限范围 YUV → RGB565（R 在高位），与 yuv_to_rgb 再截位打包逐位一致 */
static inline uint16_t yuv_to_565(int y, int u, int v)
{
    const int c = 298 * (y - 16) + 128;
    const int d = u - 128;
    const int e = v - 128;
    return (uint16_t)(k_r565[((c + 409 * e) >> 8) + CLAMP_OFS] |
                      k_g565[((c - 100 * d - 208 * e) >> 8) + CLAMP_OFS] |
                      k_b565[((c + 516 * d) >> 8) + CLAMP_OFS]);
}

void film_viewfinder_row_to_rgb888(const uint8_t *uyvy, uint32_t width, uint8_t *rgb)
{
    /* UYVY：每 4 字节是两个像素 U Y0 V Y1 */
    for (uint32_t x = 0; x + 1 < width; x += 2, uyvy += 4, rgb += 6) {
        yuv_to_rgb(uyvy[1], uyvy[0], uyvy[2], rgb);
        yuv_to_rgb(uyvy[3], uyvy[0], uyvy[2], rgb + 3);
    }
}

esp_err_t film_viewfinder_convert(const film_viewfinder_source_t *src, bool instant, uint16_t *out)
{
    if (!src || !src->uyvy || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (src->width < src->height || src->height < 2) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t vw, vh;
    film_viewfinder_size(instant, &vw, &vh);
    const size_t stride = src->stride ? src->stride : (size_t)src->width * 2u;

    /* 裁切块（传感器坐标）：全部行 × 居中的若干列，与暗房一致 */
    const uint32_t block_h = src->height;
    const uint32_t block_w = instant ? src->height
                                     : src->width * FILM_DARKROOM_M6_CROP_NUM / FILM_DARKROOM_M6_CROP_DEN;
    const uint32_t crop_x0 = (src->width - block_w) / 2u;

    for (uint32_t y0 = 0; y0 < vh; y0 += TILE_ROWS) {
        const uint32_t rows = vh - y0 < TILE_ROWS ? vh - y0 : TILE_ROWS;
        uint16_t pair_ofs[TILE_ROWS];   /*!< 该取景行对应传感器列所在 UYVY 像素对的字节偏移 */
        uint16_t luma_ofs[TILE_ROWS];   /*!< 该列 Y 分量的字节偏移 */
        for (uint32_t t = 0; t < rows; ++t) {
            const uint32_t y = y0 + t;
            const uint32_t col = crop_x0 + block_w - 1u - ((2u * y + 1u) * block_w) / (2u * vh);
            pair_ofs[t] = (uint16_t)((col >> 1) * 4u);
            luma_ofs[t] = (uint16_t)(pair_ofs[t] + 1u + (col & 1u) * 2u);
        }
        for (uint32_t x = 0; x < vw; ++x) {
            const uint32_t row = ((2u * x + 1u) * block_h) / (2u * vw);
            const uint8_t *line = src->uyvy + (size_t)row * stride;
            uint16_t *dst = out + (size_t)y0 * vw + x;
            for (uint32_t t = 0; t < rows; ++t) {
                const uint8_t *pair = line + pair_ofs[t];
                *dst = yuv_to_565(line[luma_ofs[t]], pair[0], pair[2]);
                dst += vw;
            }
        }
    }
    return ESP_OK;
}

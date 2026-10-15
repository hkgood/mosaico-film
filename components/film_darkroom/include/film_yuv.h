/*
 * film_yuv：成片的 YUV420 存放格式与 RGB 的互转。纯 C、无依赖，设备与主机测试共用。
 *
 * 全分辨率成片的 RGB888 要 9 MB 以上，PSRAM 放不下，所以成片在内存里以 12 bit/像素的 YUV420 存放（约 4.7 MB），
 * 分条带转成 RGB 做滤镜，最后整块交给硬件 JPEG 编码器（输入格式 YUV420，无需再转换）。
 *
 * 内存格式与 ESP 硬件编解码器的 "YUV420"（ESP_COLOR_FOURCC_OUYY_EVYY）一致：每两行为一组，
 *   第 1 行：[U Y0 Y1] [U Y2 Y3] ...（每两个像素 3 字节）
 *   第 2 行：[V Y0 Y1] [V Y2 Y3] ...
 * 一组占 3 × 宽 字节（宽、高按 2 向上取整）；U、V 是所在 2×2 像素块的平均。
 * 颜色转换用 JPEG（JFIF）的全范围 BT.601。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 宽高按 2 向上取整后的尺寸（缓冲按取整后的尺寸分配，多出的一列/一行是边缘像素的复制） */
static inline uint16_t film_yuv_even(uint16_t v)
{
    return (uint16_t)((v + 1u) & ~1u);
}

/** 缓冲字节数 */
static inline size_t film_yuv_bytes(uint16_t width, uint16_t height)
{
    return (size_t)film_yuv_even(width) * film_yuv_even(height) * 3 / 2;
}

/** 单个像素的颜色转换（RGB 为 0..255） */
void film_yuv_from_rgb(int r, int g, int b, uint8_t *ret_y, uint8_t *ret_u, uint8_t *ret_v);
void film_yuv_to_rgb(int y, int u, int v, uint8_t ret_rgb[3]);

/**
 * 把 RGB888（R、G、B 字节顺序，rgb_stride 为行字节数）矩形写入 YUV 缓冲的 (x, y) 处。
 * 色度按 2×2 块平均，所以要求 x、y 为偶数，且 w、h 为偶数（矩形贴着图像右/下边缘时可以是奇数）。
 * 返回 0 成功，-1 参数不满足对齐或越界。
 */
int film_yuv_put_rgb(uint8_t *yuv, uint16_t width, uint16_t height, int x, int y, int w, int h, const uint8_t *rgb,
                     size_t rgb_stride);

/** 把 YUV 缓冲的矩形读成 RGB888（任意位置和大小；色度在 2×2 内重复）。返回 0 成功，-1 越界 */
int film_yuv_get_rgb(const uint8_t *yuv, uint16_t width, uint16_t height, int x, int y, int w, int h, uint8_t *rgb,
                     size_t rgb_stride);

/** 读一个像素的 Y（缩放、统计用，不做颜色转换） */
static inline uint8_t film_yuv_luma(const uint8_t *yuv, uint16_t width, int x, int y)
{
    const size_t pair = (size_t)(y >> 1) * film_yuv_even(width) * 3;
    const size_t line = (y & 1) ? (size_t)film_yuv_even(width) * 3 / 2 : 0;
    return yuv[pair + line + (size_t)(x >> 1) * 3 + 1 + (x & 1)];
}

#ifdef __cplusplus
}
#endif

#include "film_yuv.h"

static inline int clamp255(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

void film_yuv_from_rgb(int r, int g, int b, uint8_t *ret_y, uint8_t *ret_u, uint8_t *ret_v)
{
    /* JFIF 全范围 BT.601，系数 Q8（Y 的系数和为 256，色度系数各自对称） */
    *ret_y = (uint8_t)clamp255((77 * r + 150 * g + 29 * b + 128) >> 8);
    *ret_u = (uint8_t)clamp255(((-43 * r - 85 * g + 128 * b + 128) >> 8) + 128);
    *ret_v = (uint8_t)clamp255(((128 * r - 107 * g - 21 * b + 128) >> 8) + 128);
}

void film_yuv_to_rgb(int y, int u, int v, uint8_t ret_rgb[3])
{
    const int cb = u - 128, cr = v - 128;
    ret_rgb[0] = (uint8_t)clamp255(y + ((359 * cr + 128) >> 8));
    ret_rgb[1] = (uint8_t)clamp255(y - ((88 * cb + 183 * cr + 128) >> 8));
    ret_rgb[2] = (uint8_t)clamp255(y + ((454 * cb + 128) >> 8));
}

/** 第 y 行（偶数起的行对）所在的行组首地址，以及 U 行、V 行的地址 */
static inline uint8_t *pair_base(uint8_t *yuv, uint16_t width, int y)
{
    return yuv + (size_t)(y >> 1) * film_yuv_even(width) * 3;
}

int film_yuv_put_rgb(uint8_t *yuv, uint16_t width, uint16_t height, int x, int y, int w, int h, const uint8_t *rgb,
                     size_t rgb_stride)
{
    if (!yuv || !rgb || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > width || y + h > height) {
        return -1;
    }
    if ((x & 1) || (y & 1) || ((w & 1) && x + w != width) || ((h & 1) && y + h != height)) {
        return -1;
    }
    const size_t line_bytes = (size_t)film_yuv_even(width) * 3 / 2;
    for (int by = 0; by < h; by += 2) {
        const int rows = h - by >= 2 ? 2 : 1;
        uint8_t *u_line = pair_base(yuv, width, y + by);
        uint8_t *v_line = u_line + line_bytes;
        for (int bx = 0; bx < w; bx += 2) {
            const int cols = w - bx >= 2 ? 2 : 1;
            int u_sum = 0, v_sum = 0;
            uint8_t luma[2][2];
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    /* 奇数尺寸的边缘：缺的那一行/列复制相邻像素 */
                    const uint8_t *px = rgb + (size_t)(by + (j < rows ? j : 0)) * rgb_stride +
                                        (size_t)(bx + (i < cols ? i : 0)) * 3;
                    uint8_t yy, uu, vv;
                    film_yuv_from_rgb(px[0], px[1], px[2], &yy, &uu, &vv);
                    luma[j][i] = yy;
                    u_sum += uu;
                    v_sum += vv;
                }
            }
            const size_t at = (size_t)((x + bx) >> 1) * 3;
            u_line[at] = (uint8_t)((u_sum + 2) >> 2);
            u_line[at + 1] = luma[0][0];
            u_line[at + 2] = luma[0][1];
            v_line[at] = (uint8_t)((v_sum + 2) >> 2);
            v_line[at + 1] = luma[1][0];
            v_line[at + 2] = luma[1][1];
        }
    }
    return 0;
}

int film_yuv_get_rgb(const uint8_t *yuv, uint16_t width, uint16_t height, int x, int y, int w, int h, uint8_t *rgb,
                     size_t rgb_stride)
{
    if (!yuv || !rgb || w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > width || y + h > height) {
        return -1;
    }
    const size_t line_bytes = (size_t)film_yuv_even(width) * 3 / 2;
    for (int j = 0; j < h; ++j) {
        const int yy = y + j;
        const uint8_t *u_line = yuv + (size_t)(yy >> 1) * film_yuv_even(width) * 3;
        const uint8_t *v_line = u_line + line_bytes;
        const uint8_t *line = (yy & 1) ? v_line : u_line;
        uint8_t *out = rgb + (size_t)j * rgb_stride;
        for (int i = 0; i < w; ++i, out += 3) {
            const int xx = x + i;
            const size_t at = (size_t)(xx >> 1) * 3;
            film_yuv_to_rgb(line[at + 1 + (xx & 1)], u_line[at], v_line[at], out);
        }
    }
    return 0;
}

/* 主机测试用的二进制 PPM（P6，8 位 RGB）读写 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t *pixels;    /*!< 紧凑 RGB888，调用者用 free() 释放 */
    int width;
    int height;
} ppm_image_t;

bool ppm_read(const char *path, ppm_image_t *ret_image);
bool ppm_write(const char *path, const ppm_image_t *image);

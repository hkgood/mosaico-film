/*
 * film_viewfinder：传感器 UYVY 原始帧 → 屏幕取景帧（RGB565，不加胶卷效果）。
 *
 * 裁切与转向规则和暗房成片完全相同（见 film_darkroom.c 顶部的几何约定），
 * 所以"取景框里看到的"就是"成片里得到的"，只是成片还要再冲洗。
 *
 * 纯 CPU 实现（最近邻采样 + BT.601 有限范围转换），不使用 PPA / 2D-DMA：
 * 显示刷新也要用 2D-DMA，两者并发时曾把 2D-DMA 队列卡死。
 * 无内部状态，可在任意任务调用；不在栈上放大数组。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 一帧传感器原始数据（UYVY，每像素 2 字节） */
typedef struct {
    const uint8_t *uyvy;
    uint32_t width;
    uint32_t height;
    size_t stride;          /*!< 每行字节数，0 表示紧凑排列（width * 2） */
} film_viewfinder_source_t;

/** 取景帧尺寸：M6 为 480×360，宝丽来为 360×360 */
void film_viewfinder_size(bool instant, uint16_t *ret_w, uint16_t *ret_h);

/**
 * 把一帧转换成取景帧，写入 out（紧凑排列，至少 film_viewfinder_size 给出的像素数）。
 * 传感器必须是横幅（width >= height），否则返回 ESP_ERR_INVALID_SIZE。
 */
esp_err_t film_viewfinder_convert(const film_viewfinder_source_t *src, bool instant, uint16_t *out);

/**
 * 一行 UYVY → RGB888（R、G、B 字节顺序，与取景同一套 BT.601 有限范围换算），width 须为偶数。
 * 拍照后用快门前最后一帧取景出"小样"时，把它当作 film_darkroom 的原片来源。
 */
void film_viewfinder_row_to_rgb888(const uint8_t *uyvy, uint32_t width, uint8_t *rgb);

#ifdef __cplusplus
}
#endif

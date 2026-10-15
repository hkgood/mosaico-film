/*
 * film_viewfinder：传感器 UYVY 原始帧 → 屏幕取景帧（RGB565，不加胶卷效果）。
 *
 * 裁切与转向规则和暗房成片完全相同（见 film_darkroom.c 顶部的几何约定），
 * 所以"取景框里看到的"就是"成片里得到的"，只是成片还要再冲洗。
 *
 * 可选"胶卷外观"（film_viewfinder_look_t）：只做色调曲线、饱和度和单色，不加颗粒、红晕、暗角、漏光。
 * 外观折进查找表，每像素仍是同样次数的查表，帧率代价约 0；外观由 film_filter_look() 生成。
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
#include "film_filter.h"

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

/** RGB565 查表长度：BT.601 每个分量的取值范围 [-277, 534] 加偏移后的下标范围 */
#define FILM_VF_LUT_LEN 832

/**
 * 带胶卷外观的转换表（约 9.5 KB，建议放内部 RAM）。
 * 用 film_viewfinder_look_set 填充；填充后只读，转换期间不能同时重填。
 */
typedef struct {
    bool mono;
    int32_t rv[256];                /*!< 按 V 取：409 × 缩放后的 (V-128) */
    int32_t gu[256];                /*!< 按 U 取：-100 × 缩放后的 (U-128) */
    int32_t gv[256];                /*!< 按 V 取：-208 × 缩放后的 (V-128) */
    int32_t bu[256];                /*!< 按 U 取：516 × 缩放后的 (U-128) */
    uint16_t r565[FILM_VF_LUT_LEN]; /*!< 下标是未截断的 R 分量 + 偏移，值是过曲线后的 565 位 */
    uint16_t g565[FILM_VF_LUT_LEN];
    uint16_t b565[FILM_VF_LUT_LEN];
    uint16_t gray565[256];          /*!< 单色：按传感器 Y 取 */
} film_viewfinder_look_t;

/**
 * 用胶卷外观填充转换表。look 为 NULL 表示恒等外观（结果与不带外观的转换逐位一致）。
 * 色度缩放（饱和度）也在表里完成，所以带外观的转换每像素没有乘法。
 */
void film_viewfinder_look_set(film_viewfinder_look_t *dst, const film_look_t *look);

/**
 * 取景帧尺寸：宝丽来 360×360；M6 全画幅（crop_43 为假）270×360（3:4 竖幅，屏幕取景窗口高 360），
 * 4:3 裁切 480×360 横幅。
 * 与暗房成片同一取景范围（film_darkroom_job_t.crop_43）。
 */
void film_viewfinder_size(bool instant, bool crop_43, uint16_t *ret_w, uint16_t *ret_h);

/**
 * 把一帧转换成取景帧，写入 out（紧凑排列，至少 film_viewfinder_size 给出的像素数；缓冲按 FILM_VF_M6_W × FILM_VF_M6_H 个像素分配即可）。
 * 传感器必须是横幅（width >= height），否则返回 ESP_ERR_INVALID_SIZE。
 */
esp_err_t film_viewfinder_convert(const film_viewfinder_source_t *src, bool instant, bool crop_43, uint16_t *out);

/** 同 film_viewfinder_convert，但带胶卷外观；look 为 NULL 时等同于不带外观 */
esp_err_t film_viewfinder_convert_look(const film_viewfinder_source_t *src, bool instant, bool crop_43,
                                       const film_viewfinder_look_t *look, uint16_t *out);

/**
 * 一行 UYVY → RGB888（R、G、B 字节顺序，与取景同一套 BT.601 有限范围换算），width 须为偶数。
 * 拍照后用快门前最后一帧取景出"小样"时，把它当作 film_darkroom 的原片来源。
 */
void film_viewfinder_row_to_rgb888(const uint8_t *uyvy, uint32_t width, uint8_t *rgb);

#ifdef __cplusplus
}
#endif

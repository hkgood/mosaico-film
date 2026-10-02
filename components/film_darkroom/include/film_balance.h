/*
 * 原片白平衡与亮度对齐。
 *
 * 拍照时传感器从预览模式切到 JPEG 模式，自动白平衡和曝光要重新收敛，
 * 成片原图常常比快门前的预览帧偏绿、偏暗。小样来自预览帧（颜色可信），
 * 所以成片冲洗前把原片的线性光通道均值对齐到小样，校正量随照片存档，
 * 重新冲洗时沿用。
 *
 * 校正量编码（film_balance_t），0 表示不校正（旧照片也是 0）：
 *   bit 17..16  log2(G) 的细分：再加 0..3 × 1/16 档（存在 film_photo_t.flags 里，旧照片为 0）
 *   bit 15..10  log2(R/G) × 16，有符号 6 位：-2 ~ +1.94 档，每格 1/16 档
 *   bit  9..4   log2(B/G) × 16，同上
 *   bit  3..0   log2(G)   × 4，范围 -1 ~ +2.75 档（12..15 表示 -4..-1），每格 1/4 档
 * 低 16 位存进 film_photo_t.balance。
 * 预览帧与原片都在本模块里按 gamma 2.0 换算到线性光，测量与校正用同一换算，结果自洽。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 不校正 */
#define FILM_BALANCE_NONE 0u
/** 亮度细分字段（见文件头） */
#define FILM_BALANCE_FINE_SHIFT 16
#define FILM_BALANCE_FINE_MASK  0x3u

typedef uint32_t film_balance_t;

/** 原片（裁切缩放之后、冲洗之前）的线性光通道均值，取值 0 ~ 1 */
typedef struct {
    float mean[3];          /*!< R、G、B */
    uint32_t samples;       /*!< 参与统计的抽样像素数 */
    bool valid;             /*!< 有效样本太少（几乎全黑或全过曝）时为假 */
} film_tone_stats_t;

/** 参与统计的样本按通道的 8 位值直方图（3 KB，调用者提供存放处） */
typedef struct {
    uint32_t count[3][256];
} film_tone_hist_t;

/**
 * 统计 RGB888 图像的通道均值（隔行隔列抽样，跳过接近全黑与过曝的像素）。
 * ret_hist 可为 NULL；给出时同时输出直方图，供 film_balance_match 精确求解。
 */
void film_balance_measure(const uint8_t *pixels, size_t stride, int width, int height, film_tone_stats_t *ret_stats,
                          film_tone_hist_t *ret_hist);

/**
 * 求把 from 校正到 to 的编码（超出范围时截断）；任一组无效时返回 FILM_BALANCE_NONE。
 * from_hist 为 from 的直方图时按校正曲线精确求解，为 NULL 时用均值比近似（高光多时会少补）。
 */
film_balance_t film_balance_match(const film_tone_stats_t *from, const film_tone_hist_t *from_hist,
                                  const film_tone_stats_t *to);

/** 编码 → 三通道线性增益 */
void film_balance_gains(film_balance_t code, float ret_gain[3]);

/** 按编码生成三通道查找表（lut[c][v] 为校正后的值） */
void film_balance_build_lut(film_balance_t code, uint8_t lut[3][256]);

/** 原地校正 RGB888 图像 */
void film_balance_apply(uint8_t *pixels, size_t stride, int width, int height, const uint8_t lut[3][256]);

#ifdef __cplusplus
}
#endif

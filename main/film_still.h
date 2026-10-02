/*
 * film_still：在取景暂停期间，用原生 V4L2 从 OV3640 取一张 2048×1536 JPEG。
 *
 * 传感器切换格式时寄存器表会软复位，自动曝光从头收敛（约 1 秒）、模组的白平衡调校也被冲掉。
 * 这里把取景已收敛的曝光按行时间换算搬到拍照模式并锁定，重放白平衡调校，
 * 于是开流后第二张有效帧（快门后约 0.3 秒）就是曝光正确的快门帧。结束时把曝光搬回取景模式。
 *
 * 白平衡同理来不及收敛（快门帧还停在 1.0 倍增益，偏黄绿），而自动白平衡的结果又读不出来，
 * 所以拍照模式锁定为手动增益，并闭环学习：冲洗时测得原片相对小样（预览帧，颜色可信）
 * 的残差，连同这张所用的增益一起交回 film_still_learn_awb，下一张就用修正后的增益。
 *
 * 只能在相机任务里调用，且调用前 BSP 相机必须已 stop_stream + close（释放了采集缓冲）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_balance.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILM_STILL_WIDTH  2048
#define FILM_STILL_HEIGHT 1536

/** 拍照模式的手动白平衡增益 R、G、B（0x40 = 1.0） */
typedef struct {
    uint8_t gain[3];
} film_still_awb_t;

/** 一张快门帧 */
typedef struct {
    uint8_t *jpeg;              /*!< PSRAM 中的副本，所有者用 heap_caps_free 释放 */
    size_t size;
    film_still_awb_t awb;       /*!< 这张所用的白平衡增益，冲洗后连同残差交回 film_still_learn_awb */
} film_still_t;

/** 取一张快门帧；失败时 ret->jpeg 为 NULL */
esp_err_t film_still_capture(film_still_t *ret);

/**
 * 白平衡反馈（任意任务可调用）：用 used 拍的那张原片，对齐小样所需的校正为 residual
 * （film_balance.h 编码）。下一张的增益在 used 基础上按残差的色度部分逐步修正（带阻尼），
 * 亮度交给曝光与暗房。
 */
void film_still_learn_awb(const film_still_awb_t *used, film_balance_t residual);

#ifdef __cplusplus
}
#endif

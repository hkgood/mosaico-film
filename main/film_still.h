/*
 * film_still：在取景暂停期间，用原生 V4L2 从 OV3640 取一张 JPEG（尺寸由 film_capture_mode 选，OV3640 为 2048×1536）。
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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_balance.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 拍照模式的手动白平衡增益 R、G、B（0x40 = 1.0） */
typedef struct {
    uint8_t gain[3];
} film_still_awb_t;

/** 一张快门帧 */
typedef struct {
    uint8_t *jpeg;              /*!< PSRAM 中的副本，所有者用 heap_caps_free 释放 */
    size_t size;
    film_still_awb_t awb;       /*!< 这张所用的白平衡增益，冲洗后连同残差交回 film_still_learn_awb；全 0 表示没用手动白平衡 */
    /**
     * 传感器不出 JPEG（如 SC101IOT）：jpeg 为 NULL，快门帧就是 film_camera 交出的 proof（4:3 窗口的 UYVY），
     * 暗房用硬件编码器把它编成 JPEG 原片。分辨率是取景格式的 4:3 窗口（SC101IOT 1280×720 时为 960×720）。
     */
    bool from_frame;
} film_still_t;

/** 读出当前相机传感器的芯片 ID（pid）；相机的视频设备已注册、没有别的任务在用它时调用（相机任务） */
esp_err_t film_still_sensor_pid(uint16_t *ret_pid);

/**
 * 这个传感器的 JPEG 拍照通道能不能用：要能出 JPEG，还要有本模块会控制的曝光、白平衡寄存器（只有 OV3640）。
 * 曝光读数（film_still_exposure_index）同样只在这时可用。SC101IOT 只有 YUV。
 */
bool film_still_sensor_has_jpeg(uint16_t pid);

/** 传感器的短名字，用于日志与设置页的关于信息；认不出时返回 "UNKNOWN" */
const char *film_still_sensor_name(uint16_t pid);

/** 取一张 width×height 的 JPEG 快门帧（传感器必须编进了这个 JPEG 格式）；失败时 ret->jpeg 为 NULL */
esp_err_t film_still_capture(uint16_t width, uint16_t height, film_still_t *ret);

/** film_still_exposure_index 的单位：曝光正好一帧、增益 1 倍 */
#define FILM_STILL_EXPOSURE_UNIT 16u

/**
 * 读 OV3640 自动曝光此刻的结果，折算成"场景有多暗"：(曝光行数 + 额外行) / 每帧行数 × 增益，
 * 以 FILM_STILL_EXPOSURE_UNIT 为一帧满曝光、1 倍增益。越大越暗。
 * 与 film_still_capture 不同，取景流开着时也能调用（另开一个视频设备引用，只读寄存器）；只在相机任务里调用。
 */
esp_err_t film_still_exposure_index(uint32_t *ret_index);

/**
 * 白平衡反馈（任意任务可调用）：用 used 拍的那张原片，对齐小样所需的校正为 residual
 * （film_balance.h 编码）。下一张的增益在 used 基础上按残差的色度部分逐步修正（带阻尼），
 * 亮度交给曝光与暗房。
 */
void film_still_learn_awb(const film_still_awb_t *used, film_balance_t residual);

#ifdef __cplusplus
}
#endif

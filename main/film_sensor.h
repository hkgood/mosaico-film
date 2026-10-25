/*
 * film_sensor：直接对 DVP 视频设备做的传感器格式操作——列出传感器编进固件的全部格式，切到选定的取景格式。
 *
 * 选哪个格式由 film_capture_mode 决定；这里只负责与 esp_video 打交道。
 * esp_video 在最后一个引用关闭后保留传感器格式（下次打开按它初始化），所以切换后 BSP 相机以 0×0（沿用当前格式）
 * 打开就会用上选定的格式。
 *
 * 只在相机任务里调用：BSP 相机已注册视频设备（mosaico_camera_new 之后），且取景流没开（open 之前或 close 之后）。
 */
#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "film_capture_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 列出传感器的格式，最多 capacity 个（FILM_CAPTURE_MODE_MAX 足够）；下标与 film_sensor_apply_mode 一致 */
esp_err_t film_sensor_list_modes(film_capture_mode_t *modes, size_t capacity, size_t *ret_count);

/** 把传感器切到第 index 个格式 */
esp_err_t film_sensor_apply_mode(size_t index);

#ifdef __cplusplus
}
#endif

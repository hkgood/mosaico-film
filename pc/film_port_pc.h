/*
 * PC 模拟器的平台服务：用一张样片充当传感器，冲洗、存盘走与设备相同的 film_studio；
 * 分享到手机只模拟状态变化（不开真实网络）。
 *
 * 文件格式说明：PC 上没有 JPEG 编解码器，原片和成片以 PPM 内容写在 .JPG 文件名下，
 * 只用于模拟器。设备上是真正的 JPEG。
 */
#pragma once

#include "esp_err.h"
#include "film_port.h"

typedef struct film_port_pc_t *film_port_pc_handle_t;

typedef struct {
    const char *data_dir;       /*!< 模拟器数据目录（相册、设置） */
    const char *sensor_ppm;     /*!< 传感器方向的样片 */
    const char *assets_path;    /*!< assets.bin */
} film_port_pc_config_t;

esp_err_t film_port_pc_create(const film_port_pc_config_t *config, film_port_pc_handle_t *ret_handle);
void film_port_pc_delete(film_port_pc_handle_t handle);
const film_port_t *film_port_pc_table(film_port_pc_handle_t handle);

/**
 * 换一张传感器样片（RGB888 紧凑排列，会拷贝），下一次取景就用新画面；用来录制会动的取景。
 * 冲洗任务进行中（它在读当前样片）时不替换并返回 ESP_ERR_INVALID_STATE。
 * 只能在界面线程调用（与取景同一线程）。
 */
esp_err_t film_port_pc_set_sensor(film_port_pc_handle_t handle, const uint8_t *rgb, int width, int height);

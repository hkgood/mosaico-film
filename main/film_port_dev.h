/*
 * film_port_dev：设备上的 film_port_t 实现，把界面接到各个设备服务上。
 *
 *   取景/拍照 → film_camera（核 0）    冲洗/存盘 → film_lab（核 1）
 *   分享      → film_share            声音振动  → film_feedback
 *   重力      → BMI270                设置      → NVS（nvs 分区 / film 命名空间）
 *   屏幕休眠  → film_display（亮度、关声音）
 *
 * 句柄由 main 创建并一直存活；port 表里的函数只在 GSP 渲染任务（界面线程）调用。
 */
#pragma once

#include "esp_err.h"
#include "esp_gsp.h"
#include "film_feedback.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_port_dev_t *film_port_dev_handle_t;

typedef struct {
    film_feedback_handle_t feedback;    /*!< 可为 NULL（静音） */
    esp_gsp_handle_t gsp;               /*!< 正在运行的界面：调亮度前要先暂停它 */
} film_port_dev_config_t;

/** 启动相机、暗房、存储、分享与传感器（相机缺失或存储失败不算错误，界面会提示） */
esp_err_t film_port_dev_create(const film_port_dev_config_t *config, film_port_dev_handle_t *ret_handle);
const film_port_t *film_port_dev_table(film_port_dev_handle_t handle);

#ifdef __cplusplus
}
#endif

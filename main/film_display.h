/*
 * film_display：按界面给出的屏幕状态调屏幕亮度、开关声音。
 *
 *   ON  → 亮度 80%（不提供设置，AMOLED 满亮度功耗高、画面也不更好看）
 *   DIM → 亮度 20%
 *   OFF → 亮度 0（界面同时交出一张黑画面），关掉音频编解码器
 *
 * 亮度是送给 CO5300 的面板命令，和 GSP 送帧共用一条 QSPI：必须先让 GSP 渲染任务停下再发。
 * 而暂停 GSP 要等渲染任务空闲，不能在渲染任务里做，所以由本模块的任务执行。
 * film_display_set 可在任意任务调用、立即返回；连续多次只执行最后一次。句柄由 main 创建并一直存活。
 */
#pragma once

#include "esp_err.h"
#include "esp_gsp.h"
#include "film_feedback.h"
#include "film_port.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_display_t *film_display_handle_t;

typedef struct {
    esp_gsp_handle_t gsp;               /*!< 正在运行的界面（暂停它再发面板命令） */
    film_feedback_handle_t feedback;    /*!< 可为 NULL */
    int core;
    UBaseType_t priority;
} film_display_config_t;

/** 启动任务并立即把亮度调到 ON 档 */
esp_err_t film_display_start(const film_display_config_t *config, film_display_handle_t *ret_handle);

/** 请求新的屏幕状态（异步） */
void film_display_set(film_display_handle_t handle, film_display_t state);

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
#include "film_remote_input.h"

#include <string.h>

#include "bsp/esp_mosaico.h"
#include "esp_iris.h"
#include "esp_iris_service_profiles.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* pointer/v1 报文：phase(0 按下 / 1 移动 / 2 抬起), 保留 0, x, y（int16 小端）, 保留, 序号 */
#define PHASE_RELEASE 2u
#define PHASE_MAX     2u
/*
 * 相邻两次注入至少间隔一帧：Gateway 逐点发 RPC，一整个手势约 1 ms 一点就发完；
 * 按真实手指的节奏送进界面，长按、拖动的时长判断才和真实触摸一致。
 */
#define MIN_EVENT_GAP_US (40 * 1000)

/* 单例：只在 film_remote_input_start 里写一次，之后 Iris RPC 任务只读 */
static film_shell_handle_t s_shell;
/* 上一次注入的时刻，只在 Iris RPC 任务里读写 */
static int64_t s_last_event_us;

static void pace_events(void)
{
    const int64_t wait_us = s_last_event_us + MIN_EVENT_GAP_US - esp_timer_get_time();
    if (wait_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000) + 1);
    }
    s_last_event_us = esp_timer_get_time();
}

/* Iris RPC 任务上下文 */
static esp_err_t pointer_rpc(const esp_iris_rpc_request_t *request, uint8_t *response, size_t response_capacity,
                             size_t *response_size, void *user_ctx)
{
    (void)user_ctx;
    esp_iris_status_t status = { 0 };
    esp_err_t err = esp_iris_get_status(&status);
    if (err != ESP_OK) {
        return err;
    }
    if (!status.session_ready || status.transport != ESP_IRIS_TRANSPORT_KIND_USB) {
        return ESP_ERR_NOT_ALLOWED;
    }
    if (!request || !request->payload || request->payload_size != ESP_IRIS_POINTER_MESSAGE_SIZE || !response ||
        !response_size || response_capacity < ESP_IRIS_POINTER_MESSAGE_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t *msg = request->payload;
    const int16_t x = (int16_t)(msg[2] | ((uint16_t)msg[3] << 8));
    const int16_t y = (int16_t)(msg[4] | ((uint16_t)msg[5] << 8));
    if (msg[0] > PHASE_MAX || msg[1] != 0 || x < 0 || x >= BSP_LCD_H_RES || y < 0 || y >= BSP_LCD_V_RES) {
        return ESP_ERR_INVALID_ARG;
    }
    pace_events();
    if (!film_shell_post_pointer(s_shell, x, y, msg[0] != PHASE_RELEASE)) {
        return ESP_ERR_NO_MEM;   /* 队列满：界面卡住了，丢掉这一点 */
    }
    memcpy(response, msg, ESP_IRIS_POINTER_MESSAGE_SIZE);
    *response_size = ESP_IRIS_POINTER_MESSAGE_SIZE;
    return ESP_OK;
}

esp_err_t film_remote_input_start(film_shell_handle_t shell)
{
    if (!shell) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_shell) {
        return ESP_ERR_INVALID_STATE;
    }
    s_shell = shell;
    const esp_err_t err = esp_iris_rpc_register(ESP_IRIS_POINTER_SERVICE_ID, ESP_IRIS_POINTER_METHOD_ID, pointer_rpc,
                                                NULL);
    if (err != ESP_OK) {
        s_shell = NULL;
    }
    return err;
}

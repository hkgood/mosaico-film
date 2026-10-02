// SPDX-License-Identifier: Apache-2.0
#include "film_shutter_key.h"

#include <stdint.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "film_key";

/* 单例：只在 film_shutter_key_start 里写一次，之后按键回调任务只读 */
static film_shell_handle_t s_shell;
static button_handle_t s_button;

/* 按键驱动的回调任务上下文：只投递，不碰界面状态；usr_data 区分按下/松开 */
static void on_button(void *button_handle, void *usr_data)
{
    (void)button_handle;
    const bool pressed = (uintptr_t)usr_data != 0;
    if (!film_shell_post_key(s_shell, FILM_KEY_SHUTTER, pressed)) {
        ESP_LOGW(TAG, "key queue full, %s dropped", pressed ? "press" : "release");
    }
}

esp_err_t film_shutter_key_start(film_shell_handle_t shell)
{
    ESP_RETURN_ON_FALSE(shell, ESP_ERR_INVALID_ARG, TAG, "shell");
    ESP_RETURN_ON_FALSE(!s_button, ESP_ERR_INVALID_STATE, TAG, "already started");

    button_handle_t buttons[BSP_BUTTON_NUM] = { 0 };
    ESP_RETURN_ON_ERROR(bsp_iot_button_create(buttons, NULL, BSP_BUTTON_NUM), TAG, "create AI button");
    s_shell = shell;

    /* 按下即拍：注册 PRESS_DOWN，而不是要等松手的 SINGLE_CLICK */
    esp_err_t err = iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_PRESS_DOWN, NULL, on_button, (void *)1);
    if (err == ESP_OK) {
        err = iot_button_register_cb(buttons[BSP_BUTTON_AI], BUTTON_PRESS_UP, NULL, on_button, (void *)0);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register AI button callbacks: %s", esp_err_to_name(err));
        iot_button_delete(buttons[BSP_BUTTON_AI]);
        s_shell = NULL;
        return err;
    }
    s_button = buttons[BSP_BUTTON_AI];
    return ESP_OK;
}

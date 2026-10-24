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
static button_handle_t s_flash_button;

/* usr_data 同时带上是哪个键和按下/松开：key * 2 + pressed */
#define KEY_EVENT(key, pressed) ((void *)(uintptr_t)((unsigned)(key) * 2u + ((pressed) ? 1u : 0u)))

/* 按键驱动的回调任务上下文（两个键共用同一个任务，仍是单生产者）：只投递，不碰界面状态 */
static void on_button(void *button_handle, void *usr_data)
{
    (void)button_handle;
    const uintptr_t event = (uintptr_t)usr_data;
    const film_key_t key = (film_key_t)(event / 2u);
    const bool pressed = (event & 1u) != 0;
    if (!film_shell_post_key(s_shell, key, pressed)) {
        ESP_LOGW(TAG, "key queue full, key %d %s dropped", (int)key, pressed ? "press" : "release");
    }
}

static esp_err_t register_key(button_handle_t button, film_key_t key)
{
    /* 按下即生效：注册 PRESS_DOWN，而不是要等松手的 SINGLE_CLICK */
    esp_err_t err = iot_button_register_cb(button, BUTTON_PRESS_DOWN, NULL, on_button, KEY_EVENT(key, true));
    if (err == ESP_OK) {
        err = iot_button_register_cb(button, BUTTON_PRESS_UP, NULL, on_button, KEY_EVENT(key, false));
    }
    return err;
}

/* Boot 键（GPIO61）：BSP 的按键表里没有它，按普通 GPIO 按键创建；失败只影响闪光键，快门照常 */
static void start_flash_key(void)
{
    const button_config_t common = { 0 };
    const button_gpio_config_t gpio = {
        .gpio_num = BSP_BUTTON_BOOT_GPIO, .active_level = BSP_BUTTON_ACTIVE_LEVEL,
    };
    esp_err_t err = iot_button_new_gpio_device(&common, &gpio, &s_flash_button);
    if (err == ESP_OK) {
        err = register_key(s_flash_button, FILM_KEY_FLASH);
        if (err != ESP_OK) {
            iot_button_delete(s_flash_button);
            s_flash_button = NULL;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot key (flash toggle) unavailable: %s", esp_err_to_name(err));
    }
}

esp_err_t film_shutter_key_start(film_shell_handle_t shell)
{
    ESP_RETURN_ON_FALSE(shell, ESP_ERR_INVALID_ARG, TAG, "shell");
    ESP_RETURN_ON_FALSE(!s_button, ESP_ERR_INVALID_STATE, TAG, "already started");

    button_handle_t buttons[BSP_BUTTON_NUM] = { 0 };
    ESP_RETURN_ON_ERROR(bsp_iot_button_create(buttons, NULL, BSP_BUTTON_NUM), TAG, "create AI button");
    s_shell = shell;

    const esp_err_t err = register_key(buttons[BSP_BUTTON_AI], FILM_KEY_SHUTTER);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register AI button callbacks: %s", esp_err_to_name(err));
        iot_button_delete(buttons[BSP_BUTTON_AI]);
        s_shell = NULL;
        return err;
    }
    s_button = buttons[BSP_BUTTON_AI];
    start_flash_key();
    return ESP_OK;
}

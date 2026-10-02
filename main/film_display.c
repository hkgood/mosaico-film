#include "film_display.h"

#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_log.h"
#include "freertos/task.h"

static const char *TAG = "film_display";

#define TASK_STACK          3072
#define PAUSE_TIMEOUT_MS    200     /*!< 一帧整屏传输约 61 ms，留足余量 */
#define RETRY_MS            500     /*!< 暂停界面或发命令失败后多久再试 */
#define STATE_UNKNOWN       UINT32_MAX

static const uint8_t k_brightness[] = {
    [FILM_DISPLAY_ON] = 80,
    [FILM_DISPLAY_DIM] = 20,
    [FILM_DISPLAY_OFF] = 0,
};

/* 只有内部任务读写 gsp 与 feedback；film_display_set 只发任务通知 */
struct film_display_t {
    film_display_config_t config;
    TaskHandle_t task;
};

/** 暂停界面 → 发亮度命令 → 恢复界面；再按需关开声音 */
static esp_err_t apply(film_display_handle_t h, film_display_t state)
{
    esp_gsp_esp_lcd_pause_t *pause = NULL;
    esp_err_t err = esp_gsp_esp_lcd_pause(h->config.gsp, PAUSE_TIMEOUT_MS, &pause);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pause UI: %s", esp_err_to_name(err));
        return err;
    }
    err = bsp_display_brightness_set(k_brightness[state]);
    esp_gsp_handle_t resumed = NULL;
    const esp_err_t resume_err = esp_gsp_esp_lcd_resume_paused(pause, &resumed);
    if (resume_err != ESP_OK) {
        ESP_LOGE(TAG, "resume UI: %s", esp_err_to_name(resume_err));
        return resume_err;
    }
    if (err == ESP_OK && h->config.feedback) {
        err = film_feedback_set_suspended(h->config.feedback, state == FILM_DISPLAY_OFF);
    }
    return err;
}

static void display_task(void *arg)
{
    film_display_handle_t h = arg;
    uint32_t applied = STATE_UNKNOWN;
    uint32_t wanted = FILM_DISPLAY_ON;
    for (;;) {
        if (applied != wanted) {
            if (apply(h, (film_display_t)wanted) == ESP_OK) {
                applied = wanted;
            }
        }
        /* 已经到位就一直睡；失败了隔一会儿重试（期间来的新请求会覆盖旧的） */
        const TickType_t wait = applied == wanted ? portMAX_DELAY : pdMS_TO_TICKS(RETRY_MS);
        uint32_t value;
        if (xTaskNotifyWait(0, UINT32_MAX, &value, wait) == pdTRUE) {
            wanted = value;
        }
    }
}

esp_err_t film_display_start(const film_display_config_t *config, film_display_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && config->gsp && ret_handle, ESP_ERR_INVALID_ARG, TAG, "bad args");
    film_display_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "display");
    h->config = *config;
    if (xTaskCreatePinnedToCore(display_task, "film_disp", TASK_STACK, h, config->priority, &h->task,
                                config->core) != pdPASS) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = h;
    return ESP_OK;
}

void film_display_set(film_display_handle_t handle, film_display_t state)
{
    if (handle && (unsigned)state < sizeof(k_brightness)) {
        (void)xTaskNotify(handle->task, (uint32_t)state, eSetValueWithOverwrite);
    }
}

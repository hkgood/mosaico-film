#include "film_display.h"

#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "film_display";

#define TASK_STACK          3072
#define PAUSE_TIMEOUT_MS    200     /*!< 一帧整屏传输约 61 ms，留足余量 */
#define RETRY_MS            500     /*!< 暂停界面或发命令失败后多久再试 */
#define FAILURE_LOG_PERIOD_US   (10 * 1000 * 1000)   /*!< 连续失败时多久汇总打印一次 */
#define STATE_UNKNOWN       UINT32_MAX

static const uint8_t k_brightness[] = {
    [FILM_DISPLAY_ON] = 80,
    [FILM_DISPLAY_DIM] = 20,
    [FILM_DISPLAY_OFF] = 0,
};

/* 只有内部任务读写 gsp、feedback 与 pause；film_display_set 只发任务通知 */
struct film_display_t {
    film_display_config_t config;
    TaskHandle_t task;
    /* 恢复失败时令牌仍归我们（只有成功才会被消耗），界面也仍停着：
     * 留着它下次直接重试恢复，再暂停只会得到 ESP_ERR_INVALID_STATE */
    esp_gsp_esp_lcd_pause_t *pause;
};

/**
 * 暂停界面 → 发亮度命令 → 恢复界面；再按需关开声音。
 * 失败时由 *step 带回失败的那一步，日志交给调用方限频。
 */
static esp_err_t apply(film_display_handle_t h, film_display_t state, const char **step)
{
    esp_err_t err;
    if (!h->pause) {
        err = esp_gsp_esp_lcd_pause(h->config.gsp, PAUSE_TIMEOUT_MS, &h->pause);
        if (err != ESP_OK) {
            *step = "pause UI";
            h->pause = NULL;
            return err;
        }
    }
    err = bsp_display_brightness_set(k_brightness[state]);
    esp_gsp_handle_t resumed = NULL;
    const esp_err_t resume_err = esp_gsp_esp_lcd_resume_paused(h->pause, &resumed);
    if (resume_err != ESP_OK) {
        *step = "resume UI";
        return resume_err;
    }
    h->pause = NULL;
    if (err != ESP_OK) {
        *step = "brightness";
        return err;
    }
    if (h->config.feedback) {
        err = film_feedback_set_suspended(h->config.feedback, state == FILM_DISPLAY_OFF);
        *step = "sound";
    }
    return err;
}

/**
 * 每 RETRY_MS 重试一次，失败原样打印会冲掉只有 4 KB 的 Iris 日志环：
 * 第一次失败立即打印，之后同一串失败每 FAILURE_LOG_PERIOD_US 汇总一行，成功时报告恢复。
 */
static void log_attempt(uint32_t *failures, int64_t *reported_at, esp_err_t err, const char *step)
{
    const int64_t now = esp_timer_get_time();
    if (err == ESP_OK) {
        if (*failures > 0) {
            ESP_LOGI(TAG, "display state applied after %u failed attempts", (unsigned)*failures);
        }
        *failures = 0;
        return;
    }
    ++*failures;
    if (*failures == 1 || now - *reported_at >= FAILURE_LOG_PERIOD_US) {
        ESP_LOGE(TAG, "%s: %s (%u failed attempts, retrying)", step, esp_err_to_name(err), (unsigned)*failures);
        *reported_at = now;
    }
}

static void display_task(void *arg)
{
    film_display_handle_t h = arg;
    uint32_t applied = STATE_UNKNOWN;
    uint32_t wanted = FILM_DISPLAY_ON;
    uint32_t failures = 0;
    int64_t reported_at = 0;
    for (;;) {
        if (applied != wanted) {
            const char *step = "";
            const esp_err_t err = apply(h, (film_display_t)wanted, &step);
            log_attempt(&failures, &reported_at, err, step);
            if (err == ESP_OK) {
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

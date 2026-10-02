#include "film_power.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "film_power";

#define POWER_TASK_STACK     3584
#define POWER_READ_ATTEMPTS  3      /*!< 一次读取失败时立刻重试（成功的读会清掉 i2c_master 的 NACK 残留状态） */
#define POWER_FAIL_LIMIT     3      /*!< 连续这么多次读取失败后，缓存标记为"读不到" */
#define POWER_STOP_WAIT_MS   3000   /*!< 停止时等读取任务退出的上限（初始化电量计可能要一两秒） */
#define CHARGING_CURRENT_MA  20     /*!< 充电电流为正；超过它算充电中（与 lumi_pet 一致） */

struct film_power_t {
    film_power_config_t config;
    TaskHandle_t task;
    SemaphoreHandle_t exited;   /*!< 读取任务退出前给出 */
    atomic_bool quit;

    /* 读取任务写、任意任务读：用自旋锁保护这几个字节 */
    portMUX_TYPE mux;
    film_battery_t battery;
    bool valid;
};

static void publish(film_power_handle_t h, const film_battery_t *battery, bool valid)
{
    portENTER_CRITICAL(&h->mux);
    if (battery) {
        h->battery = *battery;
    }
    h->valid = valid;
    portEXIT_CRITICAL(&h->mux);
}

static esp_err_t read_once(film_battery_t *ret_battery)
{
    bsp_battery_status_t status;
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < POWER_READ_ATTEMPTS && err != ESP_OK; ++attempt) {
        err = bsp_battery_read(&status);
    }
    if (err == ESP_OK) {
        *ret_battery = (film_battery_t){
            .percent = status.state_of_charge > 100 ? 100 : status.state_of_charge,
            .charging = status.current_ma > CHARGING_CURRENT_MA,
        };
    }
    return err;
}

static void power_task(void *arg)
{
    film_power_handle_t h = arg;
    const esp_err_t init_err = bsp_battery_init();
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable (%s): battery indicator hidden", esp_err_to_name(init_err));
    }
    int failures = 0;
    bool logged = false;
    /* 只在 film_power_stop 要求时退出：初始化失败也停在这里等，stop 才能安全地通知本任务 */
    while (!atomic_load(&h->quit)) {
        if (init_err != ESP_OK) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        film_battery_t battery;
        if (read_once(&battery) == ESP_OK) {
            failures = 0;
            publish(h, &battery, true);
            if (!logged) {
                logged = true;
                ESP_LOGI(TAG, "battery %u%%%s", (unsigned)battery.percent, battery.charging ? ", charging" : "");
            }
        } else if (++failures == POWER_FAIL_LIMIT) {
            ESP_LOGW(TAG, "battery read failed %d times in a row", failures);
            publish(h, NULL, false);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(h->config.period_ms));
    }
    xSemaphoreGive(h->exited);
    vTaskDelete(NULL);
}

esp_err_t film_power_start(const film_power_config_t *config, film_power_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && ret_handle && config->period_ms > 0, ESP_ERR_INVALID_ARG, TAG, "bad args");
    film_power_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "handle");
    h->config = *config;
    h->mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    atomic_init(&h->quit, false);
    h->exited = xSemaphoreCreateBinary();
    if (!h->exited) {
        free(h);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(power_task, "film_power", POWER_TASK_STACK, h, config->priority, &h->task,
                                config->core) != pdPASS) {
        vSemaphoreDelete(h->exited);
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = h;
    return ESP_OK;
}

bool film_power_get(film_power_handle_t h, film_battery_t *ret_battery)
{
    if (!h || !ret_battery) {
        return false;
    }
    portENTER_CRITICAL(&h->mux);
    const bool valid = h->valid;
    *ret_battery = h->battery;
    portEXIT_CRITICAL(&h->mux);
    return valid;
}

void film_power_stop(film_power_handle_t h)
{
    if (!h) {
        return;
    }
    atomic_store(&h->quit, true);
    xTaskNotifyGive(h->task);
    if (xSemaphoreTake(h->exited, pdMS_TO_TICKS(POWER_STOP_WAIT_MS)) != pdTRUE) {
        /* 任务还卡在 I2C 里：宁可泄漏这一小块，也不能在它还要访问时释放 */
        ESP_LOGE(TAG, "power task did not exit within %d ms; leaking handle", POWER_STOP_WAIT_MS);
        return;
    }
    vSemaphoreDelete(h->exited);
    free(h);
}

#include "film_power.h"

#include <stdatomic.h>
#include <stdlib.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "film_discharge.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "film_power";

#define POWER_TASK_STACK     3584
#define POWER_READ_ATTEMPTS  3      /*!< 一次读取失败时立刻重试（成功的读会清掉 i2c_master 的 NACK 残留状态） */
#define POWER_FAIL_LIMIT     3      /*!< 连续这么多次读取失败后，缓存标记为"读不到" */
#define POWER_STOP_WAIT_MS   3000   /*!< 停止时等读取任务退出的上限（初始化电量计可能要一两秒） */
#define CHARGING_CURRENT_MA  20     /*!< 充电电流为正；超过它算充电中（与 lumi_pet 一致） */
/*
 * 放电电流低于 -DISCHARGE_CURRENT_MA 才算"在用电池"。插着 USB 时充电器给整机供电，
 * 电池电流接近 0（充满）或为正（充电），这时读到的电流不代表整机功耗。
 */
#define DISCHARGE_CURRENT_MA 20
#define CPU_CORES            CONFIG_FREERTOS_NUMBER_OF_CORES

/* 各核 CPU 占用 = 1 - 空闲任务运行时间 / 墙钟时间（两次读数之间） */
typedef struct {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    configRUN_TIME_COUNTER_TYPE idle[CPU_CORES];
    configRUN_TIME_COUNTER_TYPE total;
#endif
    bool valid;
} cpu_sample_t;

struct film_power_t {
    film_power_config_t config;
    TaskHandle_t task;
    SemaphoreHandle_t exited;   /*!< 读取任务退出前给出 */
    atomic_bool quit;
    atomic_int display;         /*!< 当前屏幕状态 film_display_t，界面侧写、读取任务读 */

    /* 读取任务写、任意任务读：用自旋锁保护这几个字节 */
    portMUX_TYPE mux;
    film_battery_t battery;
    bool valid;

    /* 以下只在读取任务里使用 */
    cpu_sample_t cpu;
    film_discharge_handle_t discharge;  /*!< 放电记录；创建失败时为 NULL，只是不记 */
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

static esp_err_t read_once(bsp_battery_status_t *ret_status)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < POWER_READ_ATTEMPTS && err != ESP_OK; ++attempt) {
        err = bsp_battery_read(ret_status);
    }
    return err;
}

/* ---------------------------------------------------------------- 遥测 */

/** 返回上次调用以来各核的占用百分比；第一次调用（或未开启运行时统计）返回 false */
static bool cpu_load(cpu_sample_t *prev, unsigned ret_percent[CPU_CORES])
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    cpu_sample_t now = { .total = portGET_RUN_TIME_COUNTER_VALUE(), .valid = true };
    for (int core = 0; core < CPU_CORES; ++core) {
        now.idle[core] = ulTaskGetIdleRunTimeCounterForCore(core);
    }
    const bool ok = prev->valid && now.total != prev->total;
    if (ok) {
        const configRUN_TIME_COUNTER_TYPE span = now.total - prev->total;
        for (int core = 0; core < CPU_CORES; ++core) {
            const configRUN_TIME_COUNTER_TYPE idle = now.idle[core] - prev->idle[core];
            ret_percent[core] = idle >= span ? 0u : (unsigned)(100u - (uint64_t)idle * 100u / span);
        }
    }
    *prev = now;
    return ok;
#else
    (void)prev;
    (void)ret_percent;
    return false;
#endif
}

/** 打印一行遥测，并把这次读数整理成放电记录的样本 */
static film_discharge_sample_t telemetry(film_power_handle_t h, const bsp_battery_status_t *s)
{
    static const char *const k_display[] = { "on", "dim", "off" };
    const int display = atomic_load(&h->display);
    film_discharge_sample_t sample = {
        .average_ma = s->average_current_ma,
        .voltage_mv = s->voltage_mv,
        .soc = s->state_of_charge > 100 ? 100 : (uint8_t)s->state_of_charge,
        .display = (film_display_t)display,
    };
    unsigned load[CPU_CORES] = { 0 };
    sample.cpu_valid = cpu_load(&h->cpu, load);
    if (sample.cpu_valid) {
        sample.cpu[0] = (uint8_t)load[0];
        sample.cpu[1] = (uint8_t)load[CPU_CORES - 1];
        ESP_LOGI(TAG, "power %u%% %d mV avg %d mA %d mW | cpu0 %u%% cpu1 %u%% | display %s",
                 (unsigned)s->state_of_charge, s->voltage_mv, s->average_current_ma, s->average_power_mw, load[0],
                 load[CPU_CORES - 1], k_display[display]);
    } else {
        ESP_LOGI(TAG, "power %u%% %d mV avg %d mA %d mW | display %s", (unsigned)s->state_of_charge,
                 s->voltage_mv, s->average_current_ma, s->average_power_mw, k_display[display]);
    }
    return sample;
}

/* ---------------------------------------------------------------- 读取任务 */

static void power_task(void *arg)
{
    film_power_handle_t h = arg;
    const esp_err_t init_err = bsp_battery_init();
    if (init_err != ESP_OK) {
        ESP_LOGW(TAG, "battery gauge unavailable (%s): battery indicator hidden", esp_err_to_name(init_err));
    } else if (film_discharge_create(h->config.period_ms, &h->discharge) != ESP_OK) {
        ESP_LOGW(TAG, "discharge record unavailable");
    }
    int failures = 0;
    /* 只在 film_power_stop 要求时退出：初始化失败也停在这里等，stop 才能安全地通知本任务 */
    while (!atomic_load(&h->quit)) {
        if (init_err != ESP_OK) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        bsp_battery_status_t status;
        if (read_once(&status) == ESP_OK) {
            failures = 0;
            const film_battery_t battery = {
                .percent = status.state_of_charge > 100 ? 100 : status.state_of_charge,
                .charging = status.current_ma > CHARGING_CURRENT_MA,
            };
            publish(h, &battery, true);
            const film_discharge_sample_t sample = telemetry(h, &status);
            if (status.average_current_ma < -DISCHARGE_CURRENT_MA) {
                film_discharge_add(h->discharge, &sample);
            } else {
                film_discharge_end(h->discharge);
            }
        } else if (++failures == POWER_FAIL_LIMIT) {
            ESP_LOGW(TAG, "battery read failed %d times in a row", failures);
            publish(h, NULL, false);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(h->config.period_ms));
    }
    film_discharge_delete(h->discharge);
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
    atomic_init(&h->display, FILM_DISPLAY_ON);
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

void film_power_set_display(film_power_handle_t h, film_display_t state)
{
    if (h && (unsigned)state <= FILM_DISPLAY_OFF) {
        atomic_store(&h->display, (int)state);
    }
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

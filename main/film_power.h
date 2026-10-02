/*
 * film_power：后台低频读取电量计（BQ27220，主板共享 I2C），缓存最近一次读数给界面。
 *
 * 读电量计要十几笔 I2C 传输，放在自己的任务里低频执行，界面线程只读缓存，不碰总线。
 * 电量计初始化只尝试一次：失败（没装电池或电量计不应答）就停在"读不到"，界面隐藏电量指示，
 * 不反复探测一个会 NACK 的地址（共享总线上 NACK 之后的写传输有已知问题，见 film_port_dev.c）。
 *
 * 每次读数顺带打印功耗遥测：电压、平均电流、平均功率和各核 CPU 占用（需开启
 * CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS）和屏幕状态。只靠电池供电期间的读数交给
 * film_discharge 按屏幕状态累计并存进 NVS，接回外部电源时以 "discharge" 日志打印。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_power_t *film_power_handle_t;

typedef struct {
    int core;               /*!< 读取任务所在的核 */
    uint32_t priority;      /*!< 读取任务优先级：应高于同核的忙碌任务，免得读到一半被抢占、占着共享 I2C 总线 */
    uint32_t period_ms;     /*!< 两次读取的间隔 */
} film_power_config_t;

esp_err_t film_power_start(const film_power_config_t *config, film_power_handle_t *ret_handle);

/** 最近一次有效读数（任意任务可调用，立即返回）；还没读到或电量计不可用时返回 false */
bool film_power_get(film_power_handle_t handle, film_battery_t *ret_battery);

/** 告诉放电记录当前屏幕状态（任意任务可调用，立即返回） */
void film_power_set_display(film_power_handle_t handle, film_display_t state);

/** 停止读取任务并释放资源（会等任务退出，最多约 3 s；不能在读取任务里调用） */
void film_power_stop(film_power_handle_t handle);

#ifdef __cplusplus
}
#endif

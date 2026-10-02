/*
 * film_discharge：只靠电池供电期间的耗电记录，用来对比不同固件、不同使用状态的功耗。
 *
 * 拔掉 USB 时日志送不到主机，电池耗尽还会直接断电，所以记录边记边存进 NVS
 * （film_pwr 命名空间，每分钟写一次约 100 字节，磨损可以忽略），断电重启后接着记。
 * 接回外部电源、或者开机发现记录来自另一版固件时，把整段打印成一行 "discharge" 日志并清掉。
 *
 * 按屏幕状态（亮、暗、关）分别累计平均电流与 CPU 占用。
 * 单一所有者：只由 film_power 的读取任务调用。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_discharge_t *film_discharge_handle_t;

/** 一次读数 */
typedef struct {
    int16_t average_ma;         /*!< 电量计平均电流（放电为负） */
    int16_t voltage_mv;
    uint8_t soc;                /*!< 剩余电量 0..100 */
    film_display_t display;     /*!< 这段时间的屏幕状态 */
    bool cpu_valid;
    uint8_t cpu[2];             /*!< 两个核的占用百分比 */
} film_discharge_sample_t;

/** 读出上次没结束的记录（同一版固件就接着记，否则打印后清掉） */
esp_err_t film_discharge_create(uint32_t period_ms, film_discharge_handle_t *ret_handle);

/** 记一次放电读数 */
void film_discharge_add(film_discharge_handle_t handle, const film_discharge_sample_t *sample);

/** 接回外部电源：打印整段记录并清掉（没有记录时什么也不做） */
void film_discharge_end(film_discharge_handle_t handle);

/** 把还没写进 NVS 的读数存下来并释放（handle 可为 NULL） */
void film_discharge_delete(film_discharge_handle_t handle);

#ifdef __cplusplus
}
#endif

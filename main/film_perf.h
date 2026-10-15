/*
 * film_perf：性能打点（设备专用）。
 *
 * 由 Kconfig 开关 CONFIG_FILM_PERF_TRACE 控制，默认关闭，关闭时所有接口都是空宏，不占代码也不占时间。
 * 打开后日志标签统一为 "perf"，行首固定为 "perf <主题>"，便于 `iris logs --grep perf` 过滤、脚本解析：
 *
 *   perf shot    快门时间线（一次快门一行）：停取景 / 切格式 / 曝光帧到达 / 暗房 / 取景恢复，以及快门间隔
 *   perf mem     PSRAM 与内部 RAM：空闲、历史最小空闲、最大连续块
 *   perf cpu     自上次 film_perf_cpu_reset 以来各任务的核、优先级与 CPU 占用（FreeRTOS run-time stats）
 *   perf vf      取景帧率与每帧分段耗时：采集等待 / 转换 / 到界面取走的延迟 / 界面绘制 / canvas 提交 / 屏幕送显
 *   perf lab     暗房：小样、各阶段、JPEG 编码
 *   perf write   NAND 写入耗时与吞吐
 *
 * 线程：任意任务可调用；快门时间线按"一次只有一张在拍"设计（暗房一次只冲一张）。
 */
#pragma once

#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 快门时间线上的事件（按发生顺序） */
typedef enum {
    FILM_PERF_SHUTTER = 0,      /*!< 界面请求拍照（时间线起点，重置本次记录） */
    FILM_PERF_PROOF_GRABBED,    /*!< 小样用的取景帧副本已拿到 */
    FILM_PERF_STREAM_STOPPED,   /*!< 取景流已关闭 */
    FILM_PERF_FORMAT_SWITCHED,  /*!< 传感器已切到 JPEG，曝光与白平衡已搬运 */
    FILM_PERF_STILL_STREAMING,  /*!< 原片采集流已开 */
    FILM_PERF_FRAME_READY,      /*!< 原片 JPEG 到手（已跳过无效帧） */
    FILM_PERF_LAB_START,        /*!< 暗房开始处理这一张 */
    FILM_PERF_PROOF_DONE,       /*!< 小样已交给界面 */
    FILM_PERF_FULL_DONE,        /*!< 全尺寸冲洗结束，准备恢复取景 */
    FILM_PERF_PREVIEW_RESUMED,  /*!< 取景恢复后的第一帧已发布 */
    FILM_PERF_COUNT,
} film_perf_event_t;

#ifdef CONFIG_FILM_PERF_TRACE

#define FILM_PERF_TAG "perf"
/** 统一前缀的日志：FILM_PERF_LOG("vf", "fps %.1f", fps) → "perf vf fps 14.6" */
#define FILM_PERF_LOG(topic, fmt, ...) ESP_LOGI(FILM_PERF_TAG, topic " " fmt, ##__VA_ARGS__)
#define FILM_PERF_NOW() esp_timer_get_time()

/** 记录一个时间线事件；FILM_PERF_SHUTTER 会清空上一次记录 */
void film_perf_mark(film_perf_event_t event);
/** 相机每发布一帧取景就调用：拍照后的第一帧会结束时间线并打印一行 perf shot */
void film_perf_preview_frame(void);
/** 打印 PSRAM 与内部 RAM 的空闲、历史最小空闲和最大连续块 */
void film_perf_mem(const char *tag);
/** 以当前时刻为起点开始统计各任务 CPU 占用 */
void film_perf_cpu_reset(void);
/** 打印自上次 reset（或上次 report）以来 CPU 占用最高的任务，并把当前时刻作为新起点 */
void film_perf_cpu_report(const char *tag);

#else

/* 参数仍参与编译（免得只为打点而存在的变量报"未使用"），但永远不会执行 */
#define FILM_PERF_LOG(topic, fmt, ...)                      \
    do {                                                    \
        if (0) {                                            \
            ESP_LOGI("perf", topic " " fmt, ##__VA_ARGS__); \
        }                                                   \
    } while (0)
#define FILM_PERF_NOW() ((int64_t)0)

static inline void film_perf_mark(film_perf_event_t event) { (void)event; }
static inline void film_perf_preview_frame(void) {}
static inline void film_perf_mem(const char *tag) { (void)tag; }
static inline void film_perf_cpu_reset(void) {}
static inline void film_perf_cpu_report(const char *tag) { (void)tag; }

#endif

#ifdef __cplusplus
}
#endif

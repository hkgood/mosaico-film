/*
 * film_lab：设备上的暗房任务。拍摄与冲洗解耦：快门只负责"取到原片并落盘"，冲洗延后在后台进行。
 *
 *   拍摄：相机交来 2048×1536 JPEG 原片和快门前最后一帧取景（相机随即恢复取景，可以再拍）
 *     1. 原片落盘：带 EXIF 的 RAW 交给写盘任务，写完再写 .PND 续冲标记；照片进入待冲洗队列（最多 3 张）
 *     2. 小样：取景帧按同一套裁切/胶卷/相纸冲成屏幕图 → SHUTTER_DONE、DEVELOPED（约 0.5 s，不停取景）
 *     3. 全尺寸（延后）：距最近一次快门安静 2.5 s、RAW 写完后才开始：停取景 → 从 RAW 文件软件分块解码
 *        → film_studio（冲洗、相框、日期戳、屏幕图、缩略图）→ 硬件 JPEG 编码 → PROGRESS(DONE)，恢复取景
 *        冲洗中按快门会抢占：解码阶段放弃当前这张（RAW 与 .PND 都还在），取景恢复后立刻可拍
 *     4. 写盘（film_writer 后台任务）：JPEG/.THM/.SCR → SAVED，并删 .PND
 *   断电续冲：开机时扫描 .PND，合格的排进待冲洗队列（没有小样，冲完直接 SAVED）
 *   重新冲洗：读原片文件（另存为新照片时复制一份）→ 立刻全尺寸冲洗（界面在等结果，不延后、不抢占）；
 *     只预览时不写盘
 *
 * 线程：公共接口由界面线程（快门帧由相机任务）调用；冲洗在暗房任务里进行；结果写入 config.events 队列
 * （元素为 film_event_t，界面线程用 poll_event 取）。屏幕图缓冲由界面用 film_lab_release_screen 归还。
 * 写盘任务由暗房创建并独占使用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"
#include "film_still.h"
#include "film_viewfinder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_lab_t *film_lab_handle_t;

typedef struct {
    int core;
    int priority;
    int writer_priority;                    /*!< 后台写盘任务（不绑核） */
    QueueHandle_t events;                   /*!< film_event_t 队列，调用者拥有 */
    /**
     * 全尺寸冲洗开始/结束时调用（暗房任务上下文），设备用它暂停取景，把 CPU 和内存让给冲洗。
     * busy=true 应在取景真正停下后返回；每次 busy=true 都以 busy=false 收尾（含被快门抢占的情况）。
     * 小样与拍摄本身不调用（取景照常运行）。
     */
    void (*on_busy)(void *ctx, bool busy);
    /** 可为 NULL。返回 false 表示现在不该开始全尺寸冲洗（电量太低）：原片已落盘，下次开机再冲 */
    bool (*can_develop)(void *ctx);
    void *ctx;
} film_lab_config_t;

esp_err_t film_lab_start(const film_lab_config_t *config, film_lab_handle_t *ret_handle);

/** 设置照片目录（NULL 表示存储不可用，只允许预览） */
void film_lab_set_root(film_lab_handle_t lab, const char *root);

/** 是否还有任务在处理（含待冲洗队列） */
bool film_lab_busy(film_lab_handle_t lab);

/** 冲洗或后台写盘还没结束（不阻塞；界面据此推迟休眠） */
bool film_lab_working(film_lab_handle_t lab);

/**
 * 预约一个拍摄位。待冲洗队列没满就成功；正在拍（上一张原片还没交到）或队列满返回 ESP_ERR_INVALID_STATE。
 * 暗房正在全尺寸冲洗时会抢占它：阻塞（最多约 1.5 s，通常几百毫秒）直到取景恢复。
 */
esp_err_t film_lab_reserve(film_lab_handle_t lab);
/** 待冲洗（还没冲出成片）的照片张数，界面用来显示"还有几张在冲" */
size_t film_lab_pending(film_lab_handle_t lab);
/** 撤销 film_lab_reserve（相机没能开始拍摄时） */
void film_lab_cancel_reserve(film_lab_handle_t lab);

/**
 * 相机交来快门帧（相机任务上下文）；still->jpeg 与 proof->uyvy 的所有权都转移（heap_caps_free），
 * 失败时 still->jpeg 为 NULL；proof 可为 NULL 或 uyvy 为 NULL（没有小样，成片冲完再给屏幕图）。
 * 有小样时成片原片会对齐到小样，测得的残差连同 still->awb 反馈给 film_still_learn_awb。
 */
void film_lab_deliver_still(film_lab_handle_t lab, const film_shot_t *shot, const film_still_t *still,
                            const film_viewfinder_source_t *proof, esp_err_t err);

/** 重新冲洗；忙时返回 ESP_ERR_INVALID_STATE */
esp_err_t film_lab_redevelop(film_lab_handle_t lab, const film_shot_t *shot);

void film_lab_release_screen(film_lab_handle_t lab, const uint16_t *screen);

#ifdef __cplusplus
}
#endif

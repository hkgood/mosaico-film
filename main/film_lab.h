/*
 * film_lab：设备上的暗房任务。拍—冲串行：按下快门后立刻冲洗，冲完才能拍下一张（用户全程看着进度）。
 * 不并行的原因：全尺寸冲洗的内存峰值与取景流的帧缓冲放不进同一块 PSRAM，两个核也都被冲洗占满。
 *
 *   拍摄：相机交来 2048×1536 JPEG 原片和快门前最后一帧取景（相机受理快门时就暂停取景，一直到冲洗结束才恢复）
 *     1. 原片落盘：带 EXIF 的 RAW 交给写盘任务（重新冲洗要用它）
 *     2. 小样：取景帧按同一套裁切/胶卷/相纸冲成屏幕图 → SHUTTER_DONE、DEVELOPED（约 0.3 s）
 *     3. 全尺寸：小样出了、RAW 写完就接着冲：从 RAW 文件软件分块解码
 *        → film_studio（冲洗、相框、日期戳、屏幕图、缩略图）→ 硬件 JPEG 编码 → PROGRESS(DONE)，恢复取景
 *        失败：清掉这张照片的全部文件并发 SHOT_FAILED（不续冲）
 *     4. 写盘（film_writer 后台任务）：JPEG/.THM/.SCR → SAVED。这一步与下一张的拍摄可以重叠
 *   开机预热：LittleFS 首次分配要整盘扫描，开机 2 s 后做完它；做完之前不接快门（film_lab_reserve 报 PREPARING）
 *   重新冲洗：读原片文件（另存为新照片时复制一份）→ 立刻全尺寸冲洗；只预览时不写盘
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

/** 是否还有任务在处理（拍摄中、冲洗中、等冲洗） */
bool film_lab_busy(film_lab_handle_t lab);

/** 冲洗或后台写盘还没结束（不阻塞；界面据此推迟休眠） */
bool film_lab_working(film_lab_handle_t lab);

/**
 * 预约一个拍摄位。成功后必须交来原片（deliver_still）或 cancel。
 * 存储还在预热返回 FILM_ERR_STORAGE_PREPARING；上一张还没冲完返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t film_lab_reserve(film_lab_handle_t lab);
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

/*
 * film_writer：后台写盘任务，把照片文件写进 NAND，不占冲洗的关键路径。
 *
 * NAND 上的 LittleFS 写入只有约 100–150 KB/s，一张照片的 RAW + JPEG + THM + SCR 要写 6–8 秒。
 * 暗房把这些文件交给写盘任务后就恢复取景；写盘任务按入队顺序逐个写，每张照片以 commit 收尾：
 *   - 全部写成功：发 FILM_EVT_SAVED（界面这时才把照片加进相册）；
 *   - 有一项失败：删掉这张照片已写的文件，发 FILM_EVT_SHOT_FAILED；
 *   - 暗房自己失败（commit 带错误码）：只清理文件，不再发事件（暗房已经报过错）。
 *
 * 缓冲用 film_blob 引用计数共享：RAW 原片同时被解码器读、被写盘任务写，两边都放手后才释放。
 *
 * 线程：film_writer_* 入队函数只由暗房任务调用（单生产者）；写盘在自己的任务里进行，
 * 不绑核（取景时核 0 被相机占满，冲洗时核 1 被暗房占满，哪边空就在哪边写）。
 * 大文件按 WRITE_CHUNK 分段 fwrite，界面读缩略图时最多等一段。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_library.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 引用计数缓冲 */

typedef struct film_blob_t film_blob_t;

/** 包装一块 heap_caps 缓冲（所有权转给 blob，引用数为 1）；失败时释放 data 并返回 NULL */
film_blob_t *film_blob_wrap(void *data, size_t size);
film_blob_t *film_blob_ref(film_blob_t *blob);
/** 引用数归零时释放缓冲；NULL 安全 */
void film_blob_unref(film_blob_t *blob);
void *film_blob_data(const film_blob_t *blob);
size_t film_blob_size(const film_blob_t *blob);

/* ---------------------------------------------------------------- 写盘任务 */

typedef struct film_writer_t *film_writer_handle_t;

typedef struct {
    int priority;
    QueueHandle_t events;       /*!< film_event_t 队列，调用者拥有 */
} film_writer_config_t;

esp_err_t film_writer_start(const film_writer_config_t *config, film_writer_handle_t *ret_handle);
/** 停掉任务并释放（只用于启动失败时回滚：调用时队列必须为空） */
void film_writer_delete(film_writer_handle_t writer);

/** 写一个整文件（RAW / JPEG）；blob 的这份引用交给写盘任务。入队失败时立即放掉引用并返回错误 */
esp_err_t film_writer_file(film_writer_handle_t writer, const char *root, const film_photo_t *meta,
                           film_file_kind_t kind, film_blob_t *blob);
/** 写 .THM / .SCR（RGB565 紧凑排列，width×height）；blob 同上 */
esp_err_t film_writer_preview(film_writer_handle_t writer, const char *root, const film_photo_t *meta,
                              film_file_kind_t kind, film_blob_t *blob, uint16_t width, uint16_t height);
/**
 * 一张照片的文件都已入队。err 为 ESP_OK 时按写盘结果发 SAVED / SHOT_FAILED；
 * 否则（暗房失败）只清理已写的文件。入队失败时同步清理并返回错误。
 */
esp_err_t film_writer_commit(film_writer_handle_t writer, const char *root, const film_photo_t *meta, esp_err_t err);

/** 等到队列里的文件全部写完（暗房开始下一张大图前调用，免得两张照片的缓冲同时占内存） */
bool film_writer_wait_idle(film_writer_handle_t writer, uint32_t timeout_ms);

/** 分段写一个文件（无状态，任意任务可用）；失败时删掉半个文件。ENOSPC 映射为 FILM_ERR_STORAGE_FULL */
esp_err_t film_writer_write_path(const char *path, const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

/*
 * film_camera：相机服务（取景 + 拍照），独占 OV3640。
 *
 * 取景：UYVY 640×480 → CPU 裁切、转向、转 RGB565（film_viewfinder，不加胶卷效果），三缓冲交给界面。
 *       胶卷效果只在冲洗成片时生效；取景不占 PPA / 2D-DMA，避免与显示刷新争用。
 * 拍照：先复制一帧取景原始数据（UYVY，用来马上出小样）→ 停取景 → 传感器切到 JPEG 2048×1536
 *       （带曝光与白平衡调校）→ 取第一张曝光正确的帧 → 把 JPEG 原片和取景帧交给 on_still 回调
 *       （所有权随之转移）；取景流保持关闭直到解除暂停。
 *
 * 线程：相机任务固定在 config.core 上，所有相机操作只在该任务里发生。
 * 公共接口可在任意任务调用（内部用互斥量保护取景缓冲状态与请求）。
 * 取景帧在 film_camera_release 之前不会被改写。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"
#include "film_still.h"
#include "film_viewfinder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_camera_t *film_camera_handle_t;

/**
 * 快门帧回调（相机任务上下文）。成功时 still->jpeg 的所有权交给接收方（用 heap_caps_free 释放）；
 * 失败时 still->jpeg 为 NULL，err 说明原因。
 * proof 是快门前最后一帧取景（UYVY 640×480）：uyvy 不为 NULL 时其所有权同样交给接收方
 * （heap_caps_free），拿不到时 uyvy 为 NULL（接收方退回到用成片出屏幕图）。
 */
typedef void (*film_camera_still_cb_t)(void *ctx, const film_shot_t *shot, const film_still_t *still,
                                       const film_viewfinder_source_t *proof, esp_err_t err);

typedef struct {
    int core;
    int priority;
    film_camera_still_cb_t on_still;
    void *ctx;
} film_camera_config_t;

esp_err_t film_camera_start(const film_camera_config_t *config, film_camera_handle_t *ret_handle);

/** 相机是否已就绪并在出取景帧 */
bool film_camera_ready(film_camera_handle_t cam);
/** 更换胶卷、曝光或机身 */
void film_camera_set_preview(film_camera_handle_t cam, const film_preview_config_t *config);
/**
 * 冲洗大图时暂停取景，把 CPU 和取景流的驱动帧缓冲让给暗房（取景画面停在最后一帧）。
 * hold=true 会等到相机任务真正关掉取景流（最多约 1.5 s）再返回；hold=false 立即返回。
 * 界面第一次 film_camera_set_preview 之前也不出取景帧（开机动画期间不占 CPU）。
 */
void film_camera_hold_preview(film_camera_handle_t cam, bool hold);
/**
 * 界面暂时用不到新帧（机身选择面板盖住取景）：取景流照常运转、帧照常取回归还，
 * 只是不再转换成屏幕图，省下转换的 CPU；恢复后下一帧就有画面。与 hold_preview 互不影响。
 */
void film_camera_pause_preview(film_camera_handle_t cam, bool paused);
/** 取最新一帧（没有新帧返回 NULL） */
const film_frame_t *film_camera_acquire(film_camera_handle_t cam);
void film_camera_release(film_camera_handle_t cam, const film_frame_t *frame);
/**
 * 请求拍一张；正在拍照时返回 ESP_ERR_INVALID_STATE，相机缺失时返回 FILM_ERR_NO_CAMERA。
 * 请求成功后取景保持暂停，调用方在冲洗结束（或拍照失败）后用 film_camera_hold_preview(false) 恢复。
 */
esp_err_t film_camera_capture(film_camera_handle_t cam, const film_shot_t *shot);

#ifdef __cplusplus
}
#endif

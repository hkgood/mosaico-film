/*
 * film_camera：相机服务（取景 + 拍照），独占左侧相机板（OV3640 或 SC101IOT）。
 *
 * 连接：界面要取景后才连接。先枚举传感器格式（film_sensor），按此刻的 PSRAM 余量由 film_capture_mode
 *       选取景格式、缓冲个数与拍照来源，再切格式开流；选定的格式开不起来就退到传感器默认格式。
 *       OV3640：取景 640×480 30 fps 双缓冲，拍照 2048×1536 JPEG。
 *       SC101IOT：取景 1280×720 15 fps（PSRAM 只够单缓冲），照片是它的 4:3 窗口 960×720。
 *       每次重新连接（热插拔）都重新规划，换板子也能用上对的格式。
 * 取景：UYVY，16:9 取中间 4:3 窗口 → CPU 裁切、转向、转 RGB565（film_viewfinder，不加胶卷效果），三缓冲交给界面。
 *       胶卷效果只在冲洗成片时生效；取景不占 PPA / 2D-DMA，避免与显示刷新争用。
 * 拍照：先复制一帧取景的 4:3 窗口（UYVY，用来马上出小样）→ 停取景 → 传感器切到 JPEG 拍照格式
 *       （带曝光与白平衡调校）→ 取第一张曝光正确的帧 → 把 JPEG 原片和取景帧交给 on_still 回调
 *       （所有权随之转移）；取景流保持关闭直到解除暂停。
 *       没有 JPEG 拍照通道（SC101IOT）时没有切换格式这一步：快门帧就是复制的取景帧，见 film_still_t.from_frame。
 * 自动闪光（film_shot_t.flash_auto）：快门时测场景，够暗才点亮补光灯（GPIO34）。OV3640 读曝光寄存器，
 *       其余传感器看取景帧平均亮度。等读数在灯光下稳定后再复制小样（并切 JPEG 取快门帧），取到即关灯；
 *       这张不参与白平衡学习。
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
/**
 * 相机已连接、取景流开着，却有 2 s 以上没取到一帧（模块接触不良的典型表现）。
 * 暂停取景、拍照与冲洗期间本来就不出帧，此时返回 false。任意任务可调用。
 */
bool film_camera_preview_stalled(film_camera_handle_t cam);
/**
 * 一句话说明相机现状，给设置页的"关于"和排查"相机未连接"用：
 * 就绪时是 "OV3640 640x480"；没就绪时是原因（"NO MODULE"、"BAD EEPROM"、"NOT A CAMERA"、"ERR ..."）。
 * 任意任务可调用；cam 为 NULL 时写 "UNAVAILABLE"。
 */
void film_camera_describe(film_camera_handle_t cam, char *buf, size_t size);
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

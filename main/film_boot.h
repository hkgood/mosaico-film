/*
 * film_boot：开机动画播放器（3 秒 3D 复古相机，定格在片名）。
 *
 * 动画是资源分区里的 MFB1 包（480×480 JPEG 序列，30 fps），每帧用硬件 JPEG 解码成 RGB565。
 * 快门落下那一刻（1.6 秒）播放开机快门声与振动。
 *
 * 线程：film_boot_frame 只在 GSP 渲染任务里调用（作为 film_shell 的 intro 回调）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_feedback.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_boot_t *film_boot_handle_t;

/** data 为 MFB1 包（可在 flash 映射区里，需在播放期间一直有效） */
esp_err_t film_boot_create(const uint8_t *data, size_t size, film_feedback_handle_t feedback,
                           film_boot_handle_t *ret_handle);
void film_boot_delete(film_boot_handle_t boot);

/** film_shell_intro_t.frame 回调；ctx 为 film_boot_handle_t */
bool film_boot_frame(void *ctx, uint32_t t_ms, uint16_t *pixels);

#ifdef __cplusplus
}
#endif

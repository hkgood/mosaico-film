/*
 * film_feedback：机械相机的声音与振动（快门、拨盘、拨杆……），全部启动时合成，不占资源分区。
 *
 * 线程：film_feedback_play 可在任意任务调用（只往队列里投递）；混音与马达由内部任务独占。
 * 喇叭或马达初始化失败时只是安静地缺席，不影响相机。
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 开机动画专用：快门落下 + 胶片推进（比普通快门更厚重） */
#define FILM_FEEDBACK_BOOT ((film_feedback_t)(FILM_FEEDBACK_ERROR + 1))

typedef struct film_feedback_t *film_feedback_handle_t;

typedef struct {
    uint8_t volume_percent;
    int core;
} film_feedback_config_t;

esp_err_t film_feedback_create(const film_feedback_config_t *config, film_feedback_handle_t *ret_handle);
void film_feedback_play(film_feedback_handle_t handle, film_feedback_t kind);

#ifdef __cplusplus
}
#endif

/*
 * film_feedback：机械相机的声音与振动（快门、拨盘、拨杆……），全部启动时合成，不占资源分区。
 *
 * 线程：film_feedback_play 可在任意任务调用（只往队列里投递）；混音与马达由内部任务独占。
 * 喇叭或马达初始化失败时只是安静地缺席，不影响相机。
 */
#pragma once

#include <stdbool.h>
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

/**
 * 关屏时调用：关掉编解码器与 I2S 时钟省电（期间只有振动、没有声音）；false 时重新打开。
 * 任意任务可调用；命令排在已投递的声音之后执行。队列满到等不及时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t film_feedback_set_suspended(film_feedback_handle_t handle, bool suspended);

/**
 * 设置页的声音/振动开关；任意任务可调用，立即生效（已在响的声音放完为止）。
 * 默认两者都开。关掉声音只是不出声，喇叭与混音照常待命，重新打开无需初始化。
 */
void film_feedback_set_mode(film_feedback_handle_t handle, bool sound, bool haptic);

#ifdef __cplusplus
}
#endif

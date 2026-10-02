/*
 * film_app：Mosaico Film 的完整界面（M6 / SX-70 两种机身、暗房相册、重新冲洗、发送到手机）。
 *
 * 界面全部画在一块 480×480 RGB565 画布上，由平台推给 GSP Canvas 显示。
 * 线程：一个句柄只在一个"界面线程"上使用（单一所有者），平台服务见 film_port.h。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_gfx.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILM_APP_SCREEN_W 480
#define FILM_APP_SCREEN_H 480

typedef struct film_app_t *film_app_handle_t;

typedef struct {
    const film_port_t *port;  /*!< 平台服务表，必须在句柄生命周期内有效 */
    uint32_t seed;            /*!< 随机种子（颗粒、漏光、摇一摇换卷） */
} film_app_config_t;

esp_err_t film_app_create(const film_app_config_t *config, film_app_handle_t *ret_handle);
void film_app_delete(film_app_handle_t handle);

/** 一个触摸采样（屏幕坐标）；按下期间连续调用，抬起时 pressed = false 调用一次 */
void film_app_pointer(film_app_handle_t handle, int x, int y, bool pressed, uint32_t now_ms);

/** 实体按键 */
typedef enum {
    FILM_KEY_SHUTTER = 0,   /*!< 快门键（设备上是机身的红色 AI 键） */
} film_key_t;

/**
 * 一次实体按键变化：按下 pressed = true，松开 pressed = false。
 * 与 film_app_pointer 一样只能在独占界面的任务里调用。
 */
void film_app_key(film_app_handle_t handle, film_key_t key, bool pressed, uint32_t now_ms);

/** 推进一帧：处理平台事件、传感器与动画。返回 true 表示画面有变化需要重绘 */
bool film_app_step(film_app_handle_t handle, uint32_t now_ms);

/** 把当前画面完整绘制到画布（尺寸必须为 480×480） */
void film_app_render(film_app_handle_t handle, gfx_canvas_t *canvas);

#ifdef __cplusplus
}
#endif

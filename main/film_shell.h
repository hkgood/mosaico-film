/*
 * film_shell：把 film_app 挂到 GSP 上（设备与 PC 模拟器共用）。
 *
 *   - 场景里一张 480×480 的 Canvas 图（bind "screen"），用绘制回调把 film_app 的画面拷进去；
 *   - GSP 定时器推进 film_app_step，画面有变化时重绘并让 Canvas 刷新；
 *   - 指针观察者把触摸采样交给 film_app_pointer；
 *   - 远程触摸经 film_shell_post_pointer 进队列，下一次定时器回调时同样交给 film_app_pointer。
 *     （GSP 的 esp_gsp_inject_touch 只走 GSP 自己的控件路由，不经过指针观察者，Canvas 界面收不到。）
 *   - 实体按键经 film_shell_post_key 进另一条队列，同样在定时器回调里交给 film_app_key。
 *
 * 线程：定时器、指针观察者与绘制回调都在 GSP 渲染任务上执行，film_app 由该任务独占。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_gsp.h"
#include "film_app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct film_shell_t *film_shell_handle_t;

/**
 * 开机动画（可选）。动画期间界面还没创建；动画播完且平台服务就绪后才创建 film_app。
 * 回调都在 GSP 渲染任务上执行。
 */
typedef struct {
    /** 画第 t_ms 毫秒的一帧到 480×480 RGB565 pixels；返回 false 表示已播完（pixels 保持最后一帧） */
    bool (*frame)(void *ctx, uint32_t t_ms, uint16_t *pixels);
    /** 平台服务就绪时返回 port，否则返回 NULL（动画停在最后一帧等待） */
    const film_port_t *(*port)(void *ctx);
    void *ctx;
} film_shell_intro_t;

/** 一个统计周期内的界面绘制耗时（只统计 film_app_render，不含 Canvas 拷贝与屏幕刷新） */
typedef struct {
    uint32_t period_ms;
    uint32_t frames;                /*!< 周期内重绘了几帧 */
    uint32_t avg_us;
    uint32_t max_us;
} film_shell_stats_t;

typedef struct {
    uint16_t canvas_bind;           /*!< 场景生成的 GSP_BIND_SCREEN */
    const film_port_t *port;        /*!< 平台服务，生命周期覆盖 shell；有 intro 时忽略，由 intro->port 提供 */
    const film_shell_intro_t *intro;  /*!< 开机动画，NULL 表示直接进入界面；须在 shell 存活期间有效 */
    uint32_t seed;
    uint32_t (*now_ms)(void);       /*!< 单调毫秒时钟 */
    /** 申请/释放 480×480 RGB565 画面缓冲（设备上放 PSRAM） */
    void *(*alloc)(size_t bytes);
    void (*free)(void *ptr);
    /**
     * 可选的绘制耗时统计：两个都提供时，每个统计周期（有重绘才报）在渲染任务上调用一次 report。
     * now_us 为单调微秒时钟。
     */
    int64_t (*now_us)(void);
    void (*report)(const film_shell_stats_t *stats);
} film_shell_config_t;

esp_gsp_err_t film_shell_start(esp_gsp_handle_t ui, const film_shell_config_t *config,
                               film_shell_handle_t *ret_handle);
/**
 * 投递一个远程触摸采样（单生产者：只允许一个任务调用，比如 Iris RPC 任务）。
 * 采样在渲染任务的下一次定时器回调里按顺序交给界面；队列满时丢弃并返回 false。
 */
bool film_shell_post_pointer(film_shell_handle_t handle, int x, int y, bool pressed);

/**
 * 投递一次实体按键变化（单生产者：只允许一个任务调用，比如按键驱动的回调任务）。
 * 在渲染任务的下一次定时器回调里按顺序交给 film_app_key；开机动画期间丢弃；队列满时丢弃并返回 false。
 */
bool film_shell_post_key(film_shell_handle_t handle, film_key_t key, bool pressed);

/** 停止定时器与回调并释放资源（不能在 GSP 回调里调用；调用前须先停掉远程触摸的生产者） */
void film_shell_stop(esp_gsp_handle_t ui, film_shell_handle_t handle);

#ifdef __cplusplus
}
#endif

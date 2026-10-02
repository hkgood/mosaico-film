/*
 * 屏幕休眠：无操作 IDLE_DIM_MS 调暗并停掉取景，IDLE_OFF_MS 关屏；触摸、按键、拿起或晃动唤醒。
 *
 * 状态只在界面线程里读写。休眠中第一下触摸或按键只用来唤醒：从按下到抬起（松开）
 * 整个过程都不交给页面，免得在黑屏里误按快门。外部操作（film_app_wake）不吞输入。
 * 有正在进行的事（手指按着、拍摄或冲洗中、分享页、平台后台写盘）时一直保持亮屏。
 */
#include <string.h>

#include "app_private.h"

static void set_state(film_app_t *app, film_display_t state)
{
    if (app->power.state == state) {
        return;
    }
    app->power.state = state;
    app->power.off_drawn = false;
    app->dirty = true;   /* 醒来立刻重画；关屏时交出一张黑画面 */
    if (app->port->set_display) {
        app->port->set_display(app->port->ctx, state);
    }
}

void app_power_init(film_app_t *app)
{
    app->power.state = FILM_DISPLAY_ON;
    if (app->port->set_display) {
        app->port->set_display(app->port->ctx, FILM_DISPLAY_ON);
    }
}

void app_power_deinit(film_app_t *app)
{
    set_state(app, FILM_DISPLAY_ON);
}

void app_power_activity(film_app_t *app)
{
    app->power.active_at = app->now;
    set_state(app, FILM_DISPLAY_ON);
}

bool app_power_filter_pointer(film_app_t *app, bool pressed)
{
    if (app->power.swallow_touch) {
        app->power.swallow_touch = pressed;
        app->power.active_at = app->now;
        return true;
    }
    if (app->power.state != FILM_DISPLAY_ON) {
        if (pressed) {
            app_power_activity(app);
            app->power.swallow_touch = true;
        }
        return true;   /* 休眠中零散的抬起也丢掉 */
    }
    app->power.active_at = app->now;
    return false;
}

bool app_power_filter_key(film_app_t *app, bool pressed)
{
    if (app->power.swallow_key) {
        app->power.swallow_key = pressed;
        app->power.active_at = app->now;
        return true;
    }
    if (app->power.state != FILM_DISPLAY_ON) {
        if (pressed) {
            app_power_activity(app);
            app->power.swallow_key = true;
        }
        return true;
    }
    app->power.active_at = app->now;
    return false;
}

void app_power_motion(film_app_t *app)
{
    if (!app->power.motion_ref_valid) {
        memcpy(app->power.motion_ref, app->gravity, sizeof(app->power.motion_ref));
        app->power.motion_ref_valid = true;
        return;
    }
    float d2 = 0.0f;
    for (int i = 0; i < 3; ++i) {
        const float d = app->gravity[i] - app->power.motion_ref[i];
        d2 += d * d;
    }
    if (d2 > WAKE_MOTION_G * WAKE_MOTION_G) {
        memcpy(app->power.motion_ref, app->gravity, sizeof(app->power.motion_ref));
        app_power_activity(app);
    }
}

/** 有正在进行、不该被休眠打断的事 */
static bool busy(const film_app_t *app)
{
    const cam_state_t *cam = &app->cam;
    if (app->touch.down || app->power.swallow_touch || app->power.swallow_key) {
        return true;
    }
    if (app->screen == SCR_SHARE || cam->shot != SHOT_IDLE || cam->overlay == CAM_OVL_DEVELOPING ||
        cam->skin_swapping || app->redev.developing || app->redev.request_in_flight) {
        return true;
    }
    return app->port->keep_awake && app->port->keep_awake(app->port->ctx);
}

void app_power_step(film_app_t *app)
{
    if (!app->power.started || busy(app)) {
        app->power.started = true;
        app->power.active_at = app->now;
    }
    const uint32_t idle = app->now - app->power.active_at;
    const film_display_t target = idle >= IDLE_OFF_MS ? FILM_DISPLAY_OFF
                                : idle >= IDLE_DIM_MS ? FILM_DISPLAY_DIM
                                                      : FILM_DISPLAY_ON;
    if (target > app->power.state) {
        set_state(app, target);   /* 只往深处走；醒来由交互触发 */
    }
}

bool app_power_awake(const film_app_t *app)
{
    return app->power.state == FILM_DISPLAY_ON;
}

bool app_power_redraw(film_app_t *app, bool dirty)
{
    if (app->power.state != FILM_DISPLAY_OFF) {
        return dirty;
    }
    const bool first = !app->power.off_drawn;
    app->power.off_drawn = true;
    return first;
}

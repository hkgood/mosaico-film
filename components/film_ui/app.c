/*
 * film_app 核心：生命周期、触摸手势识别、姿态（横竖 / 水平仪 / 摇一摇）、平台事件分发、页面切换，
 * 以及各页面共用的服务（提示条、反馈、缩略图缓存、设置持久化）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_private.h"

#define GRAVITY_SMOOTHING   0.25f
#define FLAT_THRESHOLD_G    0.45f   /*!< 平放时（屏幕内重力分量太小）不改变横竖 */
#define ROT_DOMINANCE       1.35f   /*!< 主轴要比另一轴大这么多倍才切换，形成回差 */
#define ROT_SETTLE_MS       350
#define SHAKE_DELTA_G       0.85f
#define SHAKE_PEAK_GAP_MS   110
#define SHAKE_WINDOW_MS     900
#define SHAKE_COOLDOWN_MS   1600
#define VELOCITY_SMOOTHING  0.4f

static const screen_ops_t *const s_screens[SCR_COUNT] = {
    [SCR_CAMERA] = &g_screen_camera,
    [SCR_ALBUM] = &g_screen_album,
    [SCR_DETAIL] = &g_screen_detail,
    [SCR_REDEVELOP] = &g_screen_redevelop,
    [SCR_SHARE] = &g_screen_share,
    [SCR_FILM] = &g_screen_film,
};

/* ---------------------------------------------------------------- 公共服务 */

uint32_t app_random(film_app_t *app)
{
    /* xorshift32：足够做颗粒种子和随机换卷 */
    uint32_t x = app->rng ? app->rng : 0x9E3779B9U;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    app->rng = x;
    return x;
}

void app_toast(film_app_t *app, const char *text)
{
    snprintf(app->toast, sizeof(app->toast), "%s", text);
    app->toast_until = app->now + TOAST_MS;
    app->dirty = true;
}

void app_toast_error(film_app_t *app, esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        app_toast(app, "上一张还在冲洗");
        break;
    case ESP_ERR_NO_MEM:
        app_toast(app, "内存不足，请稍后再试");
        break;
    case FILM_ERR_STORAGE_FULL:
        app_toast(app, "存储空间已满");
        break;
    case FILM_ERR_NO_CAMERA:
        app_toast(app, "相机未连接");
        break;
    default:
        app_toast(app, "冲洗失败，请重试");
        break;
    }
    app_feedback(app, FILM_FEEDBACK_ERROR);
}

void app_feedback(film_app_t *app, film_feedback_t kind)
{
    if (app->port->feedback) {
        app->port->feedback(app->port->ctx, kind);
    }
}

void app_save_settings(film_app_t *app)
{
    if (app->port->settings_save) {
        app->port->settings_save(app->port->ctx, &app->settings, sizeof(app->settings));
    }
}

void app_select_film(film_app_t *app, int film)
{
    film = film_wrap(film);
    if (film == app->settings.film) {
        return;
    }
    app->settings.film = (uint8_t)film;
    app_save_settings(app);
    app_update_preview(app);
    app_feedback(app, FILM_FEEDBACK_DETENT);
}

void app_update_preview(film_app_t *app)
{
    if (!app->port->preview_config) {
        return;
    }
    const film_preview_config_t cfg = {
        .film = (film_id_t)app->settings.film,
        .exposure_ev = app->cam.ev,
        .instant = app->settings.instant != 0,
    };
    app->port->preview_config(app->port->ctx, &cfg);
}

const thumb_slot_t *app_thumb(film_app_t *app, uint32_t id)
{
    thumb_slot_t *lru = NULL;
    for (size_t i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        thumb_slot_t *s = &app->thumbs[i];
        if (s->id == id) {
            s->used = app->frame_no;
            return s->failed ? NULL : s;
        }
        if (!lru || s->used < lru->used) {
            lru = s;
        }
    }
    if (!app->library || app->thumb_loads >= THUMB_LOADS_PER_FRAME) {
        app->dirty = true;  /* 下一帧继续加载 */
        return NULL;
    }
    ++app->thumb_loads;
    lru->id = id;
    lru->used = app->frame_no;
    lru->failed = film_library_load_pixels(app->library, id, FILM_FILE_THUMB, lru->pixels,
                                           THUMB_MAX_W * THUMB_MAX_H, &lru->w, &lru->h) != ESP_OK;
    app->dirty = true;
    return lru->failed ? NULL : lru;
}

static void forget_thumb(film_app_t *app, uint32_t id)
{
    for (size_t i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        if (app->thumbs[i].id == id) {
            app->thumbs[i].id = 0;
            app->thumbs[i].used = 0;
        }
    }
}

uint32_t app_photos_bytes(film_app_t *app, const uint32_t *ids, size_t count)
{
    uint32_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        const film_photo_t *p = film_library_find(app->library, ids[i], NULL);
        if (p) {
            total += p->jpeg_bytes;
        }
    }
    return total;
}

void app_go(film_app_t *app, screen_id_t screen)
{
    if (screen >= SCR_COUNT) {
        return;
    }
    if (app->ops && app->ops->leave) {
        app->ops->leave(app);
    }
    app->screen = screen;
    app->ops = s_screens[screen];
    app->touch.last_tap_t = 0;
    if (app->ops->enter) {
        app->ops->enter(app);
    }
    app->dirty = true;
}

void app_share(film_app_t *app, const uint32_t *ids, size_t count, screen_id_t return_to)
{
    share_state_t *s = &app->share;
    s->count = count > SELECT_MAX ? SELECT_MAX : count;
    memcpy(s->ids, ids, s->count * sizeof(uint32_t));
    s->return_to = return_to;
    app_go(app, SCR_SHARE);
}

void app_open_detail(film_app_t *app, size_t index)
{
    app->detail.index = index;
    app_go(app, SCR_DETAIL);
}

void app_open_redevelop(film_app_t *app, uint32_t source_id)
{
    app->redev.source_id = source_id;
    app->redev.picking_film = false;
    app_go(app, SCR_REDEVELOP);
}

void app_open_film(film_app_t *app, screen_id_t return_to, int current)
{
    app->film.return_to = return_to == SCR_REDEVELOP ? SCR_REDEVELOP : SCR_CAMERA;
    app->film.current = film_wrap(current);
    app_go(app, SCR_FILM);
}

void app_key_back_to_camera(film_app_t *app, film_key_t key, bool pressed)
{
    if (key != FILM_KEY_SHUTTER || !pressed) {
        return;
    }
    app_feedback(app, FILM_FEEDBACK_CLICK);
    app_go(app, SCR_CAMERA);
}

void film_app_key(film_app_handle_t app, film_key_t key, bool pressed, uint32_t now_ms)
{
    if (!app) {
        return;
    }
    app->now = now_ms;
    if (app_power_filter_key(app, pressed)) {
        return;
    }
    if (app->ops->key) {
        app->ops->key(app, key, pressed);
    }
    app->dirty = true;
}

void film_app_wake(film_app_handle_t app, uint32_t now_ms)
{
    if (app) {
        app->now = now_ms;
        app_power_activity(app);
    }
}

film_display_t film_app_display(film_app_handle_t app)
{
    return app ? app->power.state : FILM_DISPLAY_ON;
}

/* ---------------------------------------------------------------- 手势识别 */

static void emit(film_app_t *app, gesture_kind_t kind)
{
    gesture_t g = {
        .kind = kind,
        .x = app->touch.x,
        .y = app->touch.y,
        .x0 = app->touch.x0,
        .y0 = app->touch.y0,
        .dx = (int16_t)(app->touch.x - app->touch.x0),
        .dy = (int16_t)(app->touch.y - app->touch.y0),
        .vx = app->touch.vx,
        .vy = app->touch.vy,
        .horizontal = app->touch.horizontal,
    };
    int du, dv;
    ui_unrot_vec(app->screen == SCR_CAMERA ? app->rot : GFX_ROT_0, g.dx, g.dy, &du, &dv);
    g.du = (int16_t)du;
    g.dv = (int16_t)dv;
    if (app->ops->gesture) {
        app->ops->gesture(app, &g);
    }
    app->dirty = true;
}

void film_app_pointer(film_app_handle_t app, int x, int y, bool pressed, uint32_t now_ms)
{
    if (!app) {
        return;
    }
    app->now = now_ms;
    if (app_power_filter_pointer(app, pressed)) {
        return;
    }
    if (pressed && !app->touch.down) {
        app->touch.down = true;
        app->touch.dragging = false;
        app->touch.long_fired = false;
        app->touch.x0 = app->touch.x = app->touch.px = (int16_t)x;
        app->touch.y0 = app->touch.y = app->touch.py = (int16_t)y;
        app->touch.t0 = app->touch.pt = now_ms;
        app->touch.vx = app->touch.vy = 0.0f;
        emit(app, GEST_PRESS);
        return;
    }
    if (pressed) {
        app->touch.x = (int16_t)x;
        app->touch.y = (int16_t)y;
        const uint32_t dt = now_ms - app->touch.pt;
        if (dt > 0) {
            const float vx = (float)(x - app->touch.px) * 1000.0f / (float)dt;
            const float vy = (float)(y - app->touch.py) * 1000.0f / (float)dt;
            app->touch.vx += (vx - app->touch.vx) * VELOCITY_SMOOTHING;
            app->touch.vy += (vy - app->touch.vy) * VELOCITY_SMOOTHING;
            app->touch.px = (int16_t)x;
            app->touch.py = (int16_t)y;
            app->touch.pt = now_ms;
        }
        const int dx = x - app->touch.x0;
        const int dy = y - app->touch.y0;
        if (!app->touch.dragging && !app->touch.long_fired && dx * dx + dy * dy > TAP_SLOP_PX * TAP_SLOP_PX) {
            int du, dv;
            ui_unrot_vec(app->screen == SCR_CAMERA ? app->rot : GFX_ROT_0, dx, dy, &du, &dv);
            app->touch.dragging = true;
            app->touch.horizontal = abs(du) >= abs(dv);
            emit(app, GEST_DRAG_BEGIN);
        }
        if (app->touch.dragging) {
            emit(app, GEST_DRAG);
        }
        return;
    }
    if (!app->touch.down) {
        return;
    }
    app->touch.down = false;
    app->touch.x = (int16_t)x;
    app->touch.y = (int16_t)y;
    if (now_ms - app->touch.pt > 80) {
        app->touch.vx = app->touch.vy = 0.0f;  /* 停住后再抬起不算甩动 */
    }
    if (app->touch.dragging) {
        emit(app, GEST_DRAG_END);
    } else if (!app->touch.long_fired) {
        emit(app, GEST_TAP);
        const int ddx = x - app->touch.last_tap_x;
        const int ddy = y - app->touch.last_tap_y;
        if (app->touch.last_tap_t && now_ms - app->touch.last_tap_t < DOUBLE_TAP_MS &&
            ddx * ddx + ddy * ddy < 40 * 40) {
            emit(app, GEST_DOUBLE_TAP);
            app->touch.last_tap_t = 0;
        } else {
            app->touch.last_tap_t = now_ms;
            app->touch.last_tap_x = (int16_t)x;
            app->touch.last_tap_y = (int16_t)y;
        }
    }
    emit(app, GEST_RELEASE);
}

static void check_long_press(film_app_t *app)
{
    if (app->touch.down && !app->touch.dragging && !app->touch.long_fired &&
        app->now - app->touch.t0 >= LONG_PRESS_MS) {
        app->touch.long_fired = true;
        emit(app, GEST_LONG);
    }
}

/* ---------------------------------------------------------------- 姿态 */

static void on_shake(film_app_t *app);

static void update_motion(film_app_t *app)
{
    float a[3];
    if (!app->port->read_accel || !app->port->read_accel(app->port->ctx, &a[0], &a[1], &a[2])) {
        return;
    }
    if (!app->gravity_valid) {
        memcpy(app->gravity, a, sizeof(a));
        app->gravity_valid = true;
    } else {
        for (int i = 0; i < 3; ++i) {
            app->gravity[i] += (a[i] - app->gravity[i]) * GRAVITY_SMOOTHING;
        }
    }
    /* 休眠中晃醒的那一下不算摇一摇换卷 */
    const bool was_awake = app_power_awake(app);
    app_power_motion(app);

    /* 摇一摇：合加速度偏离 1 g 的峰值，0.9 s 内出现 3 次 */
    const float mag = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (was_awake && fabsf(mag - 1.0f) > SHAKE_DELTA_G && app->now - app->shake_peaks[0] > SHAKE_PEAK_GAP_MS) {
        memmove(&app->shake_peaks[1], &app->shake_peaks[0], 3 * sizeof(uint32_t));
        app->shake_peaks[0] = app->now;
        if (app->shake_peaks[2] && app->now - app->shake_peaks[2] < SHAKE_WINDOW_MS &&
            app->now >= app->shake_cooldown_until) {
            app->shake_cooldown_until = app->now + SHAKE_COOLDOWN_MS;
            memset(app->shake_peaks, 0, sizeof(app->shake_peaks));
            on_shake(app);
        }
    }

    /* 横竖：重力在屏幕平面内的主方向 */
    const float gx = app->gravity[0];
    const float gy = app->gravity[1];
    const float ax = fabsf(gx);
    const float ay = fabsf(gy);
    if (sqrtf(gx * gx + gy * gy) >= FLAT_THRESHOLD_G) {
        gfx_rot_t cand = app->rot;
        if (ay > ax * ROT_DOMINANCE) {
            cand = gy > 0 ? GFX_ROT_0 : GFX_ROT_180;
        } else if (ax > ay * ROT_DOMINANCE) {
            cand = gx < 0 ? GFX_ROT_90 : GFX_ROT_270;
        }
        if (cand == app->rot) {
            app->rot_candidate = cand;
        } else if (cand != app->rot_candidate) {
            app->rot_candidate = cand;
            app->rot_candidate_since = app->now;
        } else if (app->now - app->rot_candidate_since >= ROT_SETTLE_MS) {
            app->rot = cand;
            app->dirty = true;
        }
        /* 水平仪：用户方向下重力偏离"正下方"的角度 */
        int gu, gv;
        ui_unrot_vec(app->rot, (int)(gx * 1000.0f), (int)(gy * 1000.0f), &gu, &gv);
        const float tilt = atan2f((float)gu, (float)gv) * 180.0f / 3.14159265f;
        if (fabsf(tilt - app->tilt_deg) > 0.3f) {
            app->tilt_deg = tilt;
            app->dirty = true;
        }
    }
}

/** 摇一摇：只在取景页（没有弹出层时）随机换一卷 */
static void on_shake(film_app_t *app)
{
    if (app->screen != SCR_CAMERA || app->cam.overlay != CAM_OVL_NONE || app->cam.shot != SHOT_IDLE) {
        return;
    }
    const int step = 1 + (int)(app_random(app) % (FILM_ID_COUNT - 1));
    app->settings.film = (uint8_t)film_wrap(app->settings.film + step);
    app_save_settings(app);
    app_update_preview(app);
    app_feedback(app, FILM_FEEDBACK_SHAKE);
    app_toast(app, film_info(app->settings.film)->name);
}

/* ---------------------------------------------------------------- 平台事件 */

static void handle_event(film_app_t *app, const film_event_t *ev)
{
    switch (ev->type) {
    case FILM_EVT_SAVED:
        if (app->library && film_library_add(app->library, &ev->meta) == ESP_OK) {
            forget_thumb(app, ev->meta.id);
        }
        break;
    case FILM_EVT_SHOT_FAILED:
        app_toast_error(app, ev->err);
        break;
    case FILM_EVT_STORAGE: {
        const char *root = app->port->storage_root ? app->port->storage_root(app->port->ctx) : NULL;
        if (root && !app->library) {
            const film_library_config_t cfg = { .root = root };
            app->storage_ok = film_library_create(&cfg, &app->library) == ESP_OK;
        } else if (!root && app->library) {
            film_library_delete(app->library);
            app->library = NULL;
            app->storage_ok = false;
        }
        break;
    }
    default:
        break;
    }
    if (app->ops->event) {
        app->ops->event(app, ev);
    }
    if (ev->type == FILM_EVT_DEVELOPED && ev->screen && app->port->release_result) {
        app->port->release_result(app->port->ctx, ev->screen);
    }
    app->dirty = true;
}

/* ---------------------------------------------------------------- 生命周期 */

static void load_settings(film_app_t *app)
{
    film_settings_t s = { 0 };
    if (app->port->settings_load && app->port->settings_load(app->port->ctx, &s, sizeof(s)) &&
        s.version == SETTINGS_VERSION && s.film < FILM_ID_COUNT && s.frame <= ROLL_FRAMES && s.roll > 0) {
        app->settings = s;
        return;
    }
    app->settings = (film_settings_t) {
        .version = SETTINGS_VERSION,
        .film = FILM_ID_GOLD,
        .instant = 0,
        .date_stamp = 1,
        .roll = 1,
        .frame = 0,
    };
}

static void set_preview_paused(film_app_t *app, bool paused)
{
    if (app->preview_paused == paused) {
        return;
    }
    app->preview_paused = paused;
    if (app->port->preview_pause) {
        app->port->preview_pause(app->port->ctx, paused);
    }
}

esp_err_t film_app_create(const film_app_config_t *config, film_app_handle_t *ret_handle)
{
    if (!config || !config->port || !ret_handle) {
        return ESP_ERR_INVALID_ARG;
    }
    film_app_t *app = calloc(1, sizeof(*app));
    if (!app) {
        return ESP_ERR_NO_MEM;
    }
    app->port = config->port;
    app->rng = config->seed ? config->seed : 0x2545F491U;
    app->thumb_pool = malloc(sizeof(uint16_t) * THUMB_MAX_W * THUMB_MAX_H * THUMB_CACHE_SLOTS);
    app->cam.frozen = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    app->detail.pixels = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    app->redev.original = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    app->redev.result = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    app->cam.dev_print = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
    app->share.card = malloc(sizeof(uint16_t) * SHARE_CARD_W * SHARE_CARD_H);
    app->share.media = malloc(sizeof(uint16_t) * SHARE_MEDIA_W * SHARE_MEDIA_H * SHARE_STACK_MAX);
    for (int i = 0; i < 2; ++i) {
        app->cam.stack_px[i] = malloc(sizeof(uint16_t) * CAM_STACK_PRINT_PX * CAM_STACK_PRINT_PX);
    }
    if (!app->thumb_pool || !app->cam.frozen || !app->detail.pixels || !app->redev.original || !app->redev.result ||
        !app->cam.dev_print || !app->share.card || !app->share.media || !app->cam.stack_px[0] || !app->cam.stack_px[1]) {
        film_app_delete(app);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < THUMB_CACHE_SLOTS; ++i) {
        app->thumbs[i].pixels = app->thumb_pool + i * THUMB_MAX_W * THUMB_MAX_H;
    }
    load_settings(app);
    const char *root = app->port->storage_root ? app->port->storage_root(app->port->ctx) : NULL;
    if (root) {
        const film_library_config_t cfg = { .root = root };
        app->storage_ok = film_library_create(&cfg, &app->library) == ESP_OK;
    }
    app->rot = GFX_ROT_0;
    app->rot_candidate = GFX_ROT_0;
    app->cam.film_pos = (float)app->settings.film;
    app_power_init(app);
    app_go(app, SCR_CAMERA);
    *ret_handle = app;
    return ESP_OK;
}

void film_app_delete(film_app_handle_t app)
{
    if (!app) {
        return;
    }
    app_power_deinit(app);
    if (app->ops && app->ops->leave) {
        app->ops->leave(app);
    }
    if (app->frame && app->port->preview_release) {
        app->port->preview_release(app->port->ctx, app->frame);
    }
    set_preview_paused(app, false);
    film_library_delete(app->library);
    free(app->thumb_pool);
    free(app->cam.frozen);
    free(app->detail.pixels);
    free(app->redev.original);
    free(app->redev.result);
    free(app->cam.dev_print);
    free(app->share.card);
    free(app->share.media);
    free(app->cam.stack_px[0]);
    free(app->cam.stack_px[1]);
    free(app);
}

/**
 * 取景页才取相机帧；离开取景页时把手里的帧还回去，并让平台停掉帧转换。
 * 机身选择面板打开、屏幕调暗或关闭期间画面定格在手里这一帧：不再取新帧，
 * 画面静止时整页就不用重绘，平台也停掉帧转换。
 */
static void update_preview_frame(film_app_t *app)
{
    const film_port_t *p = app->port;
    app->camera_ok = p->camera_ready ? p->camera_ready(p->ctx) : false;
    const bool on_camera = app->screen == SCR_CAMERA;
    if (!on_camera && app->frame) {
        p->preview_release(p->ctx, app->frame);
        app->frame = NULL;
    }
    set_preview_paused(app, !on_camera || app->cam.overlay == CAM_OVL_PICKER || !app_power_awake(app));
    if (app->preview_paused || !p->preview_acquire) {
        return;
    }
    const film_frame_t *f = p->preview_acquire(p->ctx);
    if (f) {
        if (app->frame) {
            p->preview_release(p->ctx, app->frame);
        }
        app->frame = f;
        app->live_dirty = true;
    }
}

/** 本帧要重画的区域：除了取景帧还有别的变化就整屏，否则只有取景框 */
static gfx_rect_t frame_damage(const film_app_t *app)
{
    const gfx_rect_t full = gfx_rect(0, 0, SCREEN_W, SCREEN_H);
    if (app->dirty || !app->ops->live_rect) {
        return full;
    }
    return app->ops->live_rect(app);
}

/** 定期取平台缓存的电量；电量很低时在取景页提示一次 */
static void update_battery(film_app_t *app)
{
    const film_port_t *p = app->port;
    if (!p->read_battery || (app->battery_polled_at && app->now - app->battery_polled_at < BATTERY_POLL_MS)) {
        return;
    }
    app->battery_polled_at = app->now ? app->now : 1;
    film_battery_t b = { 0 };
    const bool valid = p->read_battery(p->ctx, &b);
    if (b.percent > 100) {
        b.percent = 100;
    }
    if (valid != app->battery_valid || (valid && (b.percent != app->battery.percent ||
                                                  b.charging != app->battery.charging))) {
        app->battery_valid = valid;
        app->battery = b;
        app->dirty = true;
    }
    if (!valid || b.charging || b.percent > BATTERY_REARM_PERCENT) {
        app->battery_warned = false;
    } else if (!app->battery_warned && b.percent <= BATTERY_CRITICAL_PERCENT && app->screen == SCR_CAMERA) {
        app->battery_warned = true;
        app_toast(app, "电量低，请及时充电");
    }
}

bool film_app_step(film_app_handle_t app, uint32_t now_ms)
{
    if (!app) {
        return false;
    }
    const uint32_t dt = app->now ? now_ms - app->now : FRAME_PERIOD_MS;
    app->now = now_ms;
    app->thumb_loads = 0;

    film_event_t ev;
    int budget = 8;
    while (budget-- > 0 && app->port->poll_event && app->port->poll_event(app->port->ctx, &ev)) {
        handle_event(app, &ev);
    }
    check_long_press(app);
    update_motion(app);
    update_battery(app);
    app_power_step(app);
    update_preview_frame(app);
    if (app->ops->step && app->ops->step(app, dt > 200 ? 200 : dt)) {
        app->dirty = true;
    }
    if (app->toast[0]) {
        if (now_ms >= app->toast_until) {
            app->toast[0] = '\0';
        }
        app->dirty = true;
    }
    app->damage = frame_damage(app);
    const bool redraw = app_power_redraw(app, app->dirty || app->live_dirty);
    app->dirty = false;
    app->live_dirty = false;
    return redraw;
}

gfx_rect_t film_app_damage(film_app_handle_t app)
{
    return app ? app->damage : gfx_rect(0, 0, SCREEN_W, SCREEN_H);
}

void film_app_render(film_app_handle_t app, gfx_canvas_t *canvas)
{
    if (!app || !canvas) {
        return;
    }
    ++app->frame_no;
    if (app->power.state == FILM_DISPLAY_OFF) {
        gfx_fill(canvas, gfx_rect(0, 0, SCREEN_W, SCREEN_H), 0x000000, 255);
        return;
    }
    app->ops->render(app, canvas);
    if (app->toast[0]) {
        const uint32_t left = app->toast_until > app->now ? app->toast_until - app->now : 0;
        const uint8_t alpha = left > 250 ? 255 : (uint8_t)(left * 255 / 250);
        const bool camera = app->screen == SCR_CAMERA;
        const ui_frame_t f = ui_frame(camera && !app->settings.instant ? gfx_rect(0, 0, SCREEN_W, FILM_VF_M6_H)
                                      : camera ? gfx_rect(60, 0, FILM_VF_SX_W, FILM_VF_SX_H)
                                               : gfx_rect(0, 52, SCREEN_W, SCREEN_H - 52),
                                      camera ? app->rot : GFX_ROT_0);
        ui_toast(canvas, &f, app->toast, alpha);
    }
}

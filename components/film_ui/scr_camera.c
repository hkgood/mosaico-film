/*
 * 取景页：公共逻辑（快门、曝光、换卷、弹出层、一卷拍完、换机身）与 M6 旁轴机身。
 *
 * 物理布局固定（铝板始终在屏幕下方），横竖握持时只把文字、数字和取景叠加转正，
 * 手势按"用户方向"解释（竖拿时上下滑动依旧是调曝光）。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cam_private.h"

#define SHOT_FLASH_MS        70
#define SHOT_HOLD_MS         260     /*!< 定格原片的最短时间 */
#define SHOT_STUCK_MS        60000   /*!< 某一阶段等暗房的上限（事件丢失时兜底，正常不会触发） */
#define SHOT_REVEAL_MS       520     /*!< 原片 → 小样的淡入 */
#define SHOT_REVIEW_MS       1100    /*!< 小样至少停留这么久，让人看清胶卷效果 */
#define DEVELOP_PILL_W       172     /*!< 冲洗进度胶囊（用户方向，取景顶部居中） */
#define DEVELOP_PILL_H       34
#define DEVELOP_PILL_V       26      /*!< 胶囊上沿：落在亮框上沿（v = 18）之内 */
#define SHOT_FLY_MS          380
#define DEVELOP_PULSE_MS     900     /*!< "DEVELOPING" 指示灯的呼吸周期 */
#define WINDOW_PITCH         78
#define WINDOW_PITCH_V       30
#define FILM_ANIM_MS         90.0f
#define LIGHT_LEAK_CHANCE    3       /*!< 约三分之一的照片带随机漏光 */
#define LEVEL_TOLERANCE_DEG  1.5f
#define SHUTTER_HIT_R        46
#define COUNTER_HIT_R        34
/*
 * 亮框圆角：亮框上两角紧挨屏幕圆角，直角会越出圆角安全区约 15 px；
 * 半径 52 时横竖两种亮框尺寸都留 ≥ 4 px 余量（算上内外两圈淡光）。
 */
#define BRIGHTLINE_RADIUS    52

/*
 * 电量指示（用户方向，取景右上角）：百分比 + 电池图标，整体右上角相对亮框内角（M6）
 * 或取景边缘（SX-70）定位。M6 按亮框定位，竖握时亮框尺寸变化也不会压到框线；
 * 两种机身、四个握持方向都离圆角安全区 ≥ 8 px，离亮框圆弧 ≥ 7 px。
 */
#define BATTERY_BOX_W        64      /*!< "100%" + 图标的包围盒 */
#define BATTERY_BOX_H        14
#define BATTERY_BODY_W       24
#define BATTERY_BODY_H       12
#define BATTERY_NUB_W        2
#define BATTERY_NUB_H        5
#define BATTERY_TEXT_GAP     6
#define BATTERY_M6_GAP_R     16      /*!< 距亮框内角 */
#define BATTERY_M6_GAP_T     25
#define BATTERY_SX_INSET_R   28      /*!< 距取景边缘 */
#define BATTERY_SX_INSET_T   30

/* ---------------------------------------------------------------- 缓动 */

float cam_ease_out(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float cam_ease_in_out(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3.0f - 2.0f * t);
}

float cam_progress(uint32_t now, uint32_t t0, uint32_t duration)
{
    if (now <= t0) {
        return 0.0f;
    }
    const float p = (float)(now - t0) / (float)duration;
    return p > 1.0f ? 1.0f : p;
}

/** a、b 两卷之间最短的环形差值（-4..4） */
static float film_delta(float from, float to)
{
    float d = fmodf(to - from, (float)FILM_ID_COUNT);
    if (d > FILM_ID_COUNT / 2.0f) {
        d -= FILM_ID_COUNT;
    } else if (d < -FILM_ID_COUNT / 2.0f) {
        d += FILM_ID_COUNT;
    }
    return d;
}

static bool in_circle(int x, int y, int cx, int cy, int r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

/* ---------------------------------------------------------------- 动作 */

void cam_set_ev(film_app_t *app, float ev)
{
    cam_state_t *cam = &app->cam;
    ev = roundf(ev / EV_STEP) * EV_STEP;
    ev = ev < FILM_EXPOSURE_EV_MIN ? FILM_EXPOSURE_EV_MIN : (ev > FILM_EXPOSURE_EV_MAX ? FILM_EXPOSURE_EV_MAX : ev);
    cam->ev_scale_until = app->now + EV_SCALE_LINGER_MS;
    if (fabsf(ev - cam->ev) > 0.01f) {
        cam->ev = ev;
        app_feedback(app, FILM_FEEDBACK_DETENT);
        app_update_preview(app);
    }
}

void cam_open(film_app_t *app, cam_overlay_t overlay)
{
    app->cam.overlay = overlay;
    app->cam.overlay_t0 = app->now;
    app->cam.pressed_btn = 0;
}

void cam_close(film_app_t *app)
{
    app->cam.overlay = CAM_OVL_NONE;
    app->cam.overlay_t0 = app->now;
    app->cam.pressed_btn = 0;
}

void cam_switch_body(film_app_t *app, bool instant)
{
    cam_state_t *cam = &app->cam;
    if (cam->skin_swapping || instant == (app->settings.instant != 0)) {
        return;
    }
    cam->skin_swapping = true;
    cam->skin_swapped = false;
    cam->skin_t0 = app->now;
    cam->skin_target_instant = instant;
    cam_close(app);
    app_feedback(app, FILM_FEEDBACK_LEVER);
}

void cam_toggle_instant(film_app_t *app)
{
    cam_switch_body(app, app->settings.instant == 0);
}

void cam_load_new_roll(film_app_t *app, bool change_film)
{
    app->settings.roll = (uint16_t)(app->settings.roll + 1);
    app->settings.frame = 0;
    app_save_settings(app);
    app_feedback(app, FILM_FEEDBACK_CLICK);
    cam_close(app);
    if (change_film) {
        app_open_film(app, SCR_CAMERA, app->settings.film);
    }
}

void cam_fire(film_app_t *app)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay != CAM_OVL_NONE || cam->shot != SHOT_IDLE || cam->skin_swapping) {
        return;
    }
    if (!app->storage_ok || !app->library) {
        app_toast(app, "存储不可用，无法保存照片");
        app_feedback(app, FILM_FEEDBACK_ERROR);
        return;
    }
    if (!app->camera_ok) {
        app_toast_error(app, FILM_ERR_NO_CAMERA);
        return;
    }
    int64_t now_s = 0;
    const bool has_time = app->port->wall_time && app->port->wall_time(app->port->ctx, &now_s);
    film_shot_t shot = {
        .photo_id = film_library_reserve_id(app->library),
        .film = (film_id_t)app->settings.film,
        .exposure_ev = cam->ev,
        .instant = app->settings.instant != 0,
        .date_stamp = app->settings.date_stamp && has_time,
        .rot = app->rot,
        .seed = app_random(app),
        .light_leak = app_random(app) % LIGHT_LEAK_CHANCE == 0,
        .roll = app->settings.roll,
        .frame = (uint8_t)(app->settings.frame + 1),
        .time = has_time ? now_s : 0,
    };
    const esp_err_t err = app->port->shoot(app->port->ctx, &shot);
    if (err != ESP_OK) {
        app_toast_error(app, err);
        return;
    }
    app_feedback(app, FILM_FEEDBACK_SHUTTER);
    app->settings.frame++;
    app_save_settings(app);
    cam->shot_pending_rollend = app->settings.frame >= ROLL_FRAMES;
    cam->shot = SHOT_FLASH;
    cam->shot_t0 = app->now;
    cam->shot_rot = shot.rot;
    cam->dev_id = shot.photo_id;
    cam->dev_ready = false;
    cam->dev_progress = 0;
    if (shot.instant) {
        cam->dev_done_at = 0;
        cam->dev_emerge_t0 = app->now + SHOT_FLASH_MS;
        cam_open(app, CAM_OVL_DEVELOPING);
    } else if (app->frame && app->frame->width == FILM_VF_M6_W && app->frame->height == FILM_VF_M6_H) {
        memcpy(cam->frozen, app->frame->pixels, sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
        cam->frozen_w = FILM_VF_M6_W;
        cam->frozen_h = FILM_VF_M6_H;
    } else {
        cam->frozen_w = 0;
    }
}

/* ---------------------------------------------------------------- 公共绘制 */

void cam_draw_live(film_app_t *app, gfx_canvas_t *c, gfx_rect_t dst)
{
    const film_frame_t *f = app->frame;
    if (!app->camera_ok || !f) {
        gfx_fill(c, dst, 0x0B0B0A, 255);
        if (!app->camera_ok) {
            const ui_frame_t uf = ui_frame(dst, app->rot);
            const gfx_text_style_t st = ui_cjk(&font_noto_13, COLOR_MUTED, 255, GFX_ALIGN_CENTER, app->rot);
            int x, y;
            ui_point(&uf, uf.w / 2, uf.h / 2, &x, &y);
            gfx_text(c, &st, x, y, "相机未连接");
        }
        return;
    }
    /* 帧比目标大时居中裁剪 */
    const int sx = f->width > dst.w ? (f->width - dst.w) / 2 : 0;
    const int sy = f->height > dst.h ? (f->height - dst.h) / 2 : 0;
    const int w = f->width < dst.w ? f->width : dst.w;
    const int h = f->height < dst.h ? f->height : dst.h;
    gfx_copy(c, f->pixels + sy * f->width + sx, w, h, f->width, dst.x, dst.y);
}

/** 曝光刻度：用户方向的竖向刻度，右侧对齐 right，竖直居中于 cy */
static void draw_ev_scale(film_app_t *app, gfx_canvas_t *c, const ui_frame_t *f, int right, int cy)
{
    const int span = 160;
    const int top = cy - span / 2;
    for (int i = 0; i <= 12; ++i) {
        const float ev = 2.0f - (float)i * EV_STEP;
        const int v = top + i * span / 12;
        const bool major = i % 3 == 0;
        const int w = major ? 12 : 6;
        gfx_fill(c, ui_rect(f, right - w, v, w, 1), 0xFFFAEC, 217);
        if (major) {
            char label[8];
            ui_format_ev(ev, label, sizeof(label));
            const gfx_text_style_t st =
                ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.05f, 0xFFFAEC, 235, GFX_ALIGN_RIGHT, f->rot);
            int x, y;
            ui_point(f, right - 17, v, &x, &y);
            gfx_text(c, &st, x, y, label);
        }
    }
    /* 指针：尖朝左的琥珀三角 */
    const int pv = top + (int)lroundf((2.0f - app->cam.ev) / 4.0f * (float)span);
    int ax, ay, bx, by, tx, ty;
    ui_point(f, right + 5, pv - 5, &ax, &ay);
    ui_point(f, right + 5, pv + 5, &bx, &by);
    ui_point(f, right - 3, pv, &tx, &ty);
    gfx_glow(c, tx, ty, 9, COLOR_AMBER, 90);
    gfx_triangle_q4(c, ax * 16, ay * 16, bx * 16, by * 16, tx * 16, ty * 16, COLOR_AMBER, 255);
}

/** 旁轴取景框（圆角亮框 + 四个缺口刻线，刻线落在圆弧之后的直线段上） */
static void draw_brightlines(gfx_canvas_t *c, const ui_frame_t *f, int ix, int iy, int fw, int fh)
{
    const uint32_t line = 0xFFFAEC;
    const int r = BRIGHTLINE_RADIUS;
    gfx_stroke_round(c, ui_rect(f, ix - 1, iy - 1, fw + 2, fh + 2), r + 1, 1, 0xFFECC8, 50);
    gfx_stroke_round(c, ui_rect(f, ix + 1, iy + 1, fw - 2, fh - 2), r - 1, 1, 0xFFECC8, 40);
    gfx_stroke_round(c, ui_rect(f, ix, iy, fw, fh), r, 1, line, 224);
    gfx_fill(c, ui_rect(f, ix - 1, iy + r + 8, 6, 1), line, 224);
    gfx_fill(c, ui_rect(f, ix + fw - 5, iy + fh - r - 15, 6, 1), line, 224);
    gfx_fill(c, ui_rect(f, ix + r + 8, iy + fh - 5, 1, 6), line, 224);
    gfx_fill(c, ui_rect(f, ix + fw - r - 15, iy - 1, 1, 6), line, 224);
}

static uint32_t battery_color(const film_battery_t *b)
{
    if (b->charging || b->percent > BATTERY_LOW_PERCENT) {
        return COLOR_CREAM;
    }
    return b->percent > BATTERY_CRITICAL_PERCENT ? COLOR_AMBER : COLOR_LED;
}

/** 闪电（用户坐标，中心 cu, cv）：两个三角形拼成 */
static void draw_bolt(gfx_canvas_t *c, const ui_frame_t *f, int cu, int cv, uint32_t color)
{
    static const int8_t k_tris[2][6] = {
        { 2, -5, -3, 1, 1, 1 },     /* 上半：右上尖 → 左中 → 中 */
        { -2, 5, 3, -1, -1, -1 },   /* 下半：左下尖 → 右中 → 中 */
    };
    for (int t = 0; t < 2; ++t) {
        int p[3][2];
        for (int i = 0; i < 3; ++i) {
            ui_point(f, cu + k_tris[t][2 * i], cv + k_tris[t][2 * i + 1], &p[i][0], &p[i][1]);
        }
        gfx_triangle_q4(c, p[0][0] * 16, p[0][1] * 16, p[1][0] * 16, p[1][1] * 16, p[2][0] * 16, p[2][1] * 16, color,
                        255);
    }
}

/**
 * 电量指示：右上角 (right, top) 为用户坐标。暗色描边垫在文字与图标下面，
 * 亮场景里也看得清；百分比右对齐到图标左侧。
 */
static void draw_battery(film_app_t *app, gfx_canvas_t *c, const ui_frame_t *f, int right, int top)
{
    const film_battery_t *b = &app->battery;
    const uint32_t color = battery_color(b);
    const int left = right - BATTERY_BOX_W;
    ui_safe_check(ui_rect(f, left, top, BATTERY_BOX_W, BATTERY_BOX_H), 0, "battery");

    const int nub_u = right - BATTERY_NUB_W;
    const int body_u = nub_u - BATTERY_BODY_W;
    const int body_v = top + (BATTERY_BOX_H - BATTERY_BODY_H) / 2;
    const gfx_rect_t body = ui_rect(f, body_u, body_v, BATTERY_BODY_W, BATTERY_BODY_H);
    const gfx_rect_t nub = ui_rect(f, nub_u, body_v + (BATTERY_BODY_H - BATTERY_NUB_H) / 2, BATTERY_NUB_W,
                                   BATTERY_NUB_H);
    gfx_stroke_round(c, gfx_rect(body.x - 1, body.y - 1, body.w + 2, body.h + 2), 4, 3, COLOR_BLACK, 90);
    gfx_stroke_round(c, body, 3, 1, color, 255);
    gfx_fill(c, nub, color, 255);
    const int inner_w = BATTERY_BODY_W - 4;
    const int level_w = b->percent ? (inner_w * b->percent + 99) / 100 : 0;
    if (level_w > 0) {
        gfx_fill_round(c, ui_rect(f, body_u + 2, body_v + 2, level_w, BATTERY_BODY_H - 4), 1, color, 255);
    }
    static const int8_t k_halo[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
    if (b->charging) {
        /* 闪电压在电量条上：先垫一圈深色轮廓，琥珀色才不会和奶油色电量条糊在一起 */
        const int cu = body_u + BATTERY_BODY_W / 2;
        const int cv = body_v + BATTERY_BODY_H / 2;
        for (int i = 0; i < 4; ++i) {
            draw_bolt(c, f, cu + k_halo[i][0], cv + k_halo[i][1], COLOR_INK);
        }
        draw_bolt(c, f, cu, cv, COLOR_AMBER);
    }

    char text[8];
    snprintf(text, sizeof(text), "%u%%", (unsigned)b->percent);
    int tx, ty;
    ui_point(f, body_u - BATTERY_TEXT_GAP, top + BATTERY_BOX_H / 2, &tx, &ty);
    gfx_text_style_t st = ui_style(&font_jost_m10, 10, 0.04f, COLOR_BLACK, 110, GFX_ALIGN_RIGHT, f->rot);
    for (int i = 0; i < 4; ++i) {
        gfx_text(c, &st, tx + k_halo[i][0], ty + k_halo[i][1], text);
    }
    st.color = color;
    st.alpha = 255;
    gfx_text(c, &st, tx, ty, text);
}

/** 水平仪：▶ ● ◀ 三个红色指示灯 */
static void draw_level(film_app_t *app, gfx_canvas_t *c, const ui_frame_t *f, int cu, int cv)
{
    const float tilt = app->tilt_deg;
    const bool left_on = app->gravity_valid && tilt < -LEVEL_TOLERANCE_DEG;
    const bool right_on = app->gravity_valid && tilt > LEVEL_TOLERANCE_DEG;
    const bool level_on = app->gravity_valid && !left_on && !right_on;
    const int l = cu - 20;
    const int r = cu + 21;
    int p[3][2];
    /* ▶：竖边在左 */
    ui_point(f, l, cv - 5, &p[0][0], &p[0][1]);
    ui_point(f, l, cv + 5, &p[1][0], &p[1][1]);
    ui_point(f, l + 8, cv, &p[2][0], &p[2][1]);
    if (left_on) {
        gfx_glow(c, (p[0][0] + p[1][0] + p[2][0]) / 3, (p[0][1] + p[1][1] + p[2][1]) / 3, 10, COLOR_LED, 120);
    }
    gfx_triangle_q4(c, p[0][0] * 16, p[0][1] * 16, p[1][0] * 16, p[1][1] * 16, p[2][0] * 16, p[2][1] * 16, COLOR_LED,
                    left_on ? 255 : 71);
    /* ◀ */
    ui_point(f, r, cv - 5, &p[0][0], &p[0][1]);
    ui_point(f, r, cv + 5, &p[1][0], &p[1][1]);
    ui_point(f, r - 8, cv, &p[2][0], &p[2][1]);
    if (right_on) {
        gfx_glow(c, (p[0][0] + p[1][0] + p[2][0]) / 3, (p[0][1] + p[1][1] + p[2][1]) / 3, 10, COLOR_LED, 120);
    }
    gfx_triangle_q4(c, p[0][0] * 16, p[0][1] * 16, p[1][0] * 16, p[1][1] * 16, p[2][0] * 16, p[2][1] * 16, COLOR_LED,
                    right_on ? 255 : 71);
    /* ● */
    int dx, dy;
    ui_point(f, cu, cv, &dx, &dy);
    if (level_on) {
        gfx_glow(c, dx, dy, 10, COLOR_LED, 140);
    }
    gfx_circle_q4(c, dx * 16 + 8, dy * 16 + 8, 56, COLOR_LED, level_on ? 255 : 71);
    /* 曝光补偿读数 */
    if (fabsf(app->cam.ev) > 0.01f) {
        char ev[8];
        ui_format_ev(app->cam.ev, ev, sizeof(ev));
        int tx, ty;
        ui_point(f, cu + 32, cv, &tx, &ty);
        ui_glow_text(c, &font_jost_m11, &font_jost_m11_glow, 11, 0.1f, COLOR_LED, 170, GFX_ALIGN_LEFT, f->rot, tx, ty,
                     ev);
    }
}

void cam_draw_vf_overlay(film_app_t *app, gfx_canvas_t *c, gfx_rect_t vf, bool rangefinder)
{
    const ui_frame_t f = ui_frame(vf, app->rot);
    const bool landscape = f.w >= f.h;
    const int ix = rangefinder ? (landscape ? 22 : 18) : 24;
    const int iy = rangefinder ? (landscape ? 18 : 20) : 24;
    const int fw = f.w - 2 * ix;
    const int fh = rangefinder ? f.h - 48 : f.h - 2 * iy;
    if (rangefinder) {
        draw_brightlines(c, &f, ix, iy, fw, fh);
        /* 测距黄斑：取景中心的暖色小窗 */
        const gfx_rect_t patch = ui_rect(&f, f.w / 2 - 32, iy + fh / 2 - 21, 64, 42);
        gfx_fill(c, patch, 0xFFE2B8, 30);
        gfx_stroke_round(c, patch, 0, 1, 0xFFECC8, 70);
        draw_level(app, c, &f, f.w / 2, f.h - 20);
    }
    if (app->battery_valid) {
        if (rangefinder) {
            draw_battery(app, c, &f, ix + fw - BATTERY_M6_GAP_R, iy + BATTERY_M6_GAP_T);
        } else {
            draw_battery(app, c, &f, f.w - BATTERY_SX_INSET_R, BATTERY_SX_INSET_T);
        }
    }
    /* 回看小样时日期已印在照片上，不再叠一层取景提示 */
    if (app->settings.date_stamp && app->cam.shot != SHOT_REVEAL) {
        int64_t now_s = 0;
        char stamp[16];
        if (app->port->wall_time && app->port->wall_time(app->port->ctx, &now_s)) {
            ui_format_stamp(now_s, stamp, sizeof(stamp));
            int x, y;
            ui_point(&f, f.w - (rangefinder ? (landscape ? 36 : 32) : 26), f.h - (rangefinder ? 50 : 34), &x, &y);
            ui_glow_text(c, &font_dseg_20, &font_dseg_20_glow, 20, 0.14f, COLOR_STAMP, 200, GFX_ALIGN_RIGHT, f.rot, x,
                         y, stamp);
        }
    }
    if (app->cam.dragging_ev || app->now < app->cam.ev_scale_until) {
        draw_ev_scale(app, c, &f, f.w - 28, iy + fh / 2);
    }
}

void cam_draw_popover(film_app_t *app, gfx_canvas_t *c, int x)
{
    const int y = POPOVER_Y;
    ui_sprite(c, &img_popover, IMG_POPOVER_OX, IMG_POPOVER_OY, x, y, 255);
    const gfx_text_style_t title =
        ui_style(&font_jost_m14, UI_TEXT_LINK, 0.12f, COLOR_ENGRAVE, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    const gfx_text_style_t hl =
        ui_style(&font_jost_m14, UI_TEXT_LINK, 0.12f, COLOR_WHITE, 178, GFX_ALIGN_LEFT, GFX_ROT_0);
    const gfx_text_style_t sub =
        ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.06f, 0x6B6964, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &hl, x + 18, y + 25, "INSTANT");
    gfx_text(c, &title, x + 18, y + 24, "INSTANT");
    gfx_text(c, &sub, x + 18, y + 40, "1 : 1  SX-70 BODY");
    gfx_text(c, &hl, x + 18, y + 70, "DATE");
    gfx_text(c, &title, x + 18, y + 69, "DATE");
    ui_lever(c, x + 152, y + 30, app->settings.instant != 0);
    ui_lever(c, x + 152, y + 68, app->settings.date_stamp != 0);
}

void cam_draw_rollend(film_app_t *app, gfx_canvas_t *c)
{
    gfx_fill(c, gfx_rect(0, 0, SCREEN_W, SCREEN_H), COLOR_BLACK, 156);
    const gfx_rect_t card = gfx_rect(72, 64, 336, 260);
    gfx_shadow(c, gfx_rect(card.x, card.y + 8, card.w, card.h), 24, 18, COLOR_BLACK, 170);
    gfx_fill_round(c, card, 24, 0x0D0D0C, 248);
    gfx_stroke_round(c, card, 24, 1, COLOR_WHITE, 26);

    gfx_ring_q4(c, 240 * 16, 128 * 16, 52 * 16, 7 * 16, 0xD7D2C5, 255);
    gfx_ring_q4(c, 240 * 16, 128 * 16, 28 * 16, 2 * 16, 0x5D5A53, 255);
    char n[8];
    snprintf(n, sizeof(n), "%02u", (unsigned)app->settings.frame);
    ui_glow_text(c, &font_jost_m25, &font_jost_m25_glow, 25, 0.02f, COLOR_AMBER, 115, GFX_ALIGN_CENTER, GFX_ROT_0,
                 240, 128, n);
    const gfx_text_style_t title =
        ui_style(&font_jost_m14, 14, 0.26f, COLOR_CREAM, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &title, 240, 183, "ROLL COMPLETE");
    char line[64];
    snprintf(line, sizeof(line), "%u FRAMES  \xC2\xB7  %s  \xC2\xB7  ROLL %02u", (unsigned)app->settings.frame,
             film_info(app->settings.film)->name, (unsigned)app->settings.roll);
    const gfx_text_style_t st =
        ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.12f, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &st, 240, 203, line);
    ui_button_primary(c, gfx_rect(120, 224, 240, 40), "LOAD NEW ROLL", NULL, app->cam.pressed_btn == 1, true);
    ui_button_line(c, gfx_rect(120, 272, 240, 34), "CHANGE FILM", NULL, app->cam.pressed_btn == 2, COLOR_CREAM, 71);
}

void cam_draw_skin_swap(film_app_t *app, gfx_canvas_t *c)
{
    const cam_state_t *cam = &app->cam;
    if (!cam->skin_swapping) {
        return;
    }
    const uint32_t el = app->now - cam->skin_t0;
    const float p = el < SKIN_SWAP_MS ? cam_ease_in_out((float)el / SKIN_SWAP_MS)
                                      : 1.0f - cam_ease_in_out((float)(el - SKIN_SWAP_MS) / SKIN_SWAP_MS);
    const int w = (int)lroundf(p * (SCREEN_W / 2 + 2));
    if (w <= 0) {
        return;
    }
    gfx_tile(c, &img_tex_leather, gfx_rect(0, 0, w, SCREEN_H), 0, 0, 0);
    gfx_tile(c, &img_tex_leather, gfx_rect(SCREEN_W - w, 0, w, SCREEN_H), 0, 0, 0);
    gfx_gradient_h(c, gfx_rect(w - 8, 0, 8, SCREEN_H), COLOR_BLACK, 0, COLOR_BLACK, 150);
    gfx_gradient_h(c, gfx_rect(SCREEN_W - w, 0, 8, SCREEN_H), COLOR_BLACK, 150, COLOR_BLACK, 0);
    if (p > 0.72f) {
        const uint8_t alpha = (uint8_t)lroundf(fminf(1.0f, (p - 0.72f) / 0.28f) * 255.0f);
        gfx_ring_q4(c, 240 * 16, 240 * 16, 18 * 16, 2 * 16, COLOR_AMBER, alpha);
        const float a = (float)(app->now % 800) * 6.2831853f / 800.0f;
        const int dx = (int)lroundf(cosf(a) * 18.0f);
        const int dy = (int)lroundf(sinf(a) * 18.0f);
        gfx_glow(c, 240 + dx, 240 + dy, 5, COLOR_AMBER, (uint8_t)(alpha / 2));
        gfx_circle_q4(c, (240 + dx) * 16, (240 + dy) * 16, 3 * 16, COLOR_AMBER, alpha);
        const gfx_text_style_t st =
            ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.22f, COLOR_CREAM, alpha, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &st, 240, 292, "LOADING BODY");
    }
}

bool cam_overlay_gesture(film_app_t *app, const gesture_t *g, int popover_x)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay == CAM_OVL_POPOVER) {
        const gfx_rect_t row1 = gfx_rect(popover_x, POPOVER_Y, POPOVER_W, POPOVER_H / 2);
        const gfx_rect_t row2 = gfx_rect(popover_x, POPOVER_Y + POPOVER_H / 2, POPOVER_W, POPOVER_H / 2);
        if (g->kind == GEST_TAP) {
            if (ui_hit(row1, g->x, g->y, 0)) {
                cam_toggle_instant(app);
            } else if (ui_hit(row2, g->x, g->y, 0)) {
                app->settings.date_stamp = !app->settings.date_stamp;
                app_save_settings(app);
                app_feedback(app, FILM_FEEDBACK_LEVER);
            } else {
                cam_close(app);
            }
        }
        return true;
    }
    if (cam->overlay == CAM_OVL_ROLLEND) {
        const gfx_rect_t load = gfx_rect(120, 224, 240, 40);
        const gfx_rect_t change = gfx_rect(120, 272, 240, 34);
        if (g->kind == GEST_PRESS) {
            cam->pressed_btn = ui_hit(load, g->x, g->y, 4) ? 1 : (ui_hit(change, g->x, g->y, 4) ? 2 : 0);
        } else if (g->kind == GEST_TAP) {
            if (cam->pressed_btn == 1 && ui_hit(load, g->x, g->y, 8)) {
                cam_load_new_roll(app, false);
            } else if (cam->pressed_btn == 2 && ui_hit(change, g->x, g->y, 8)) {
                cam_load_new_roll(app, true);
            }
        } else if (g->kind == GEST_RELEASE) {
            cam->pressed_btn = 0;
        }
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------- M6 绘制 */

/** 胶卷窗里的胶卷名（横拿横排、竖拿竖排，始终正立） */
static void draw_window_names(film_app_t *app, gfx_canvas_t *c)
{
    const gfx_rect_t win = gfx_rect(M6_WINDOW_X - M6_WINDOW_W / 2, M6_WINDOW_Y - M6_WINDOW_H / 2, M6_WINDOW_W,
                                    M6_WINDOW_H);
    const gfx_rect_t inner = gfx_rect(win.x + 13, win.y, win.w - 26, win.h);
    const gfx_rect_t saved = gfx_clip_push(c, inner);
    const float pos = app->cam.film_pos;
    const int base = (int)floorf(pos + 0.5f);
    const bool vertical = app->rot == GFX_ROT_90 || app->rot == GFX_ROT_270;
    const ui_frame_t f = ui_frame(win, app->rot);
    for (int k = -2; k <= 2; ++k) {
        const int film = base + k;
        const float d = (float)film - pos;
        const bool sel = fabsf(d) < 0.5f;
        const film_info_t *info = film_info(film);
        if (!vertical) {
            const int x = M6_WINDOW_X + (int)lroundf(d * WINDOW_PITCH);
            const float edge = fminf((float)(x - inner.x), (float)(inner.x + inner.w - x)) / 34.0f;
            const float fade = edge < 0 ? 0 : (edge > 1 ? 1 : edge);
            const gfx_text_style_t st =
                ui_style(sel ? &font_jost_m14 : &font_jost_m11, sel ? 14 : 11, 0.08f, sel ? 0xF6F1E4 : COLOR_CREAM,
                         (uint8_t)(fade * (sel ? 255.0f : 97.0f)), GFX_ALIGN_CENTER, app->rot);
            gfx_text(c, &st, x, M6_WINDOW_Y, info->name);
        } else {
            const int v = f.h / 2 + (int)lroundf(d * WINDOW_PITCH_V);
            const float edge = fminf((float)(v - 13), (float)(f.h - 13 - v)) / 18.0f;
            const float fade = edge < 0 ? 0 : (edge > 1 ? 1 : edge);
            int x, y;
            if (sel) {
                const gfx_text_style_t w = ui_style(&font_jost_m10, 10, 0.02f, 0xF6F1E4, (uint8_t)(fade * 255.0f),
                                                    GFX_ALIGN_CENTER, app->rot);
                const gfx_text_style_t n = ui_style(&font_jost_m9, 9, 0.04f, COLOR_CREAM, (uint8_t)(fade * 180.0f),
                                                    GFX_ALIGN_CENTER, app->rot);
                ui_point(&f, f.w / 2, v - 6, &x, &y);
                gfx_text(c, &w, x, y, info->word);
                ui_point(&f, f.w / 2, v + 7, &x, &y);
                gfx_text(c, &n, x, y, info->number);
            } else {
                const gfx_text_style_t w = ui_style(&font_jost_m9, 9, 0.02f, COLOR_CREAM, (uint8_t)(fade * 97.0f),
                                                    GFX_ALIGN_CENTER, app->rot);
                ui_point(&f, f.w / 2, v, &x, &y);
                gfx_text(c, &w, x, y, info->word);
            }
        }
    }
    gfx_clip_pop(c, saved);
}

/** 部件下方的刻字：用户方向下，相对部件中心偏移 offset */
static void engrave_below(gfx_canvas_t *c, int cx, int cy, int offset, const char *text, bool light, gfx_rot_t rot)
{
    int dx, dy;
    ui_rot_vec(rot, 0, offset, &dx, &dy);
    ui_engrave(c, cx + dx, cy + dy, text, light, rot);
}

static void draw_m6_controls(film_app_t *app, gfx_canvas_t *c)
{
    const cam_state_t *cam = &app->cam;
    const gfx_rot_t rot = app->rot;
    const bool vertical = rot == GFX_ROT_90 || rot == GFX_ROT_270;

    /* 计数窗（兼相册入口） */
    ui_sprite(c, &img_counter56, IMG_COUNTER56_OX, IMG_COUNTER56_OY, M6_COUNTER_X - 28, M6_COUNTER_Y - 28, 255);
    char n[8];
    snprintf(n, sizeof(n), "%02u", (unsigned)app->settings.frame);
    ui_glow_text(c, &font_jost_m19, &font_jost_m19_glow, 19, 0.02f, COLOR_AMBER, cam->counter_down ? 200 : 115,
                 GFX_ALIGN_CENTER, rot, M6_COUNTER_X, M6_COUNTER_Y, n);
    engrave_below(c, M6_COUNTER_X, M6_COUNTER_Y, M6_LABEL_GAP, "ROLL", false, rot);

    /* 滚花快门 */
    if (cam->shutter_down) {
        ui_sprite(c, &img_shutter_down, IMG_SHUTTER_DOWN_OX, IMG_SHUTTER_DOWN_OY, M6_SHUTTER_X - 42, M6_SHUTTER_Y - 42,
                  255);
    } else {
        ui_sprite(c, &img_shutter, IMG_SHUTTER_OX, IMG_SHUTTER_OY, M6_SHUTTER_X - 42, M6_SHUTTER_Y - 42, 255);
    }

    /* 胶卷窗 */
    const int wx = M6_WINDOW_X - M6_WINDOW_W / 2;
    const int wy = M6_WINDOW_Y - M6_WINDOW_H / 2;
    if (cam->window_down || cam->overlay == CAM_OVL_POPOVER) {
        ui_sprite(c, &img_win140_glow, IMG_WIN140_GLOW_OX, IMG_WIN140_GLOW_OY, wx, wy, 255);
    }
    ui_sprite(c, &img_win140_base, IMG_WIN140_BASE_OX, IMG_WIN140_BASE_OY, wx, wy, 255);
    draw_window_names(app, c);
    ui_sprite(c, &img_win140_over, IMG_WIN140_OVER_OX, IMG_WIN140_OVER_OY, wx, wy, 255);
    const int half = vertical ? M6_WINDOW_W / 2 : M6_WINDOW_H / 2;
    int dx, dy;
    ui_rot_vec(rot, 0, -(half + 9), &dx, &dy);
    ui_index_mark(c, M6_WINDOW_X + dx, M6_WINDOW_Y + dy, rot);
    engrave_below(c, M6_WINDOW_X, M6_WINDOW_Y, vertical ? half + M6_LABEL_GAP_V : M6_LABEL_GAP, "FILM", false, rot);
}

/**
 * 把转正后的屏幕成片转回取景方向，写成 480×360（与取景帧逐像素对齐，便于淡入对比）。
 * 映射规则与 ui_point 相同；竖拿时成片只有 270×360，最近邻放大回取景尺寸。
 */
static void print_to_view(const uint16_t *src, int w, int h, gfx_rot_t rot, uint16_t *dst)
{
    const int vw = FILM_VF_M6_W;
    const int vh = FILM_VF_M6_H;
    const bool swap = rot == GFX_ROT_90 || rot == GFX_ROT_270;
    const int uw = swap ? vh : vw;      /* 转正后（用户方向）的虚拟画幅 */
    const int uh = swap ? vw : vh;
    for (int y = 0; y < vh; ++y) {
        for (int x = 0; x < vw; ++x) {
            int u, v;
            switch (rot) {
            case GFX_ROT_90:
                u = y;
                v = vw - 1 - x;
                break;
            case GFX_ROT_180:
                u = vw - 1 - x;
                v = vh - 1 - y;
                break;
            case GFX_ROT_270:
                u = vh - 1 - y;
                v = x;
                break;
            default:
                u = x;
                v = y;
                break;
            }
            dst[y * vw + x] = src[(v * h / uh) * w + u * w / uw];
        }
    }
}

/**
 * 拍后"正在冲洗"提示：用户方向顶部居中的暗色胶囊，里面是琥珀灯 + 字 + 真实进度条。
 * 呼吸灯只在暗房还没报进度时闪（通常不到 1 秒）；之后灯常亮，画面只在进度变化时重绘，
 * 把 CPU 留给冲洗。
 */
static void draw_developing(film_app_t *app, gfx_canvas_t *c, gfx_rect_t vf)
{
    const cam_state_t *cam = &app->cam;
    const ui_frame_t f = ui_frame(vf, app->rot);
    const bool done = cam->dev_progress >= FILM_PROGRESS_DONE;
    uint8_t glow = 200;
    if (!cam->dev_progress) {
        const float phase = (float)((app->now - cam->shot_t0) % DEVELOP_PULSE_MS) / DEVELOP_PULSE_MS;
        glow = (uint8_t)(90.0f + 140.0f * (0.5f - 0.5f * cosf(phase * 6.2831853f)));
    }
    char label[24];
    if (done) {
        snprintf(label, sizeof(label), "DEVELOPED");
    } else if (cam->dev_progress) {
        snprintf(label, sizeof(label), "DEVELOPING  %u%%", (unsigned)(cam->dev_progress / 10));
    } else {
        snprintf(label, sizeof(label), "DEVELOPING");
    }
    /* 暗色胶囊垫底：亮画面上字和进度条也看得清 */
    const int u0 = (f.w - DEVELOP_PILL_W) / 2;
    const int v0 = DEVELOP_PILL_V;
    gfx_fill_round(c, ui_rect(&f, u0, v0, DEVELOP_PILL_W, DEVELOP_PILL_H), 8, COLOR_BLACK, 150);

    int dx, dy, tx, ty;
    ui_point(&f, u0 + 15, v0 + 13, &dx, &dy);
    ui_point(&f, u0 + 26, v0 + 13, &tx, &ty);
    gfx_glow(c, dx, dy, 9, COLOR_AMBER, glow);
    gfx_circle_q4(c, dx * 16 + 8, dy * 16 + 8, 40, COLOR_AMBER, 255);
    ui_glow_text(c, &font_jost_m11, &font_jost_m11_glow, 11, 0.22f, COLOR_AMBER, 150, GFX_ALIGN_LEFT, f.rot, tx, ty,
                 label);

    /* 进度条：浅色底槽 + 琥珀色填充，头部带一点光晕 */
    const int bar_u = u0 + 14;
    const int bar_v = v0 + DEVELOP_PILL_H - 9;
    const int bar_w = DEVELOP_PILL_W - 28;
    const int fill = (int)((uint32_t)bar_w * cam->dev_progress / FILM_PROGRESS_DONE);
    gfx_fill(c, ui_rect(&f, bar_u, bar_v, bar_w, 2), 0xFFFAEC, 50);
    if (fill > 0) {
        gfx_fill(c, ui_rect(&f, bar_u, bar_v, fill, 2), COLOR_AMBER, 255);
        int gx, gy;
        ui_point(&f, bar_u + fill, bar_v + 1, &gx, &gy);
        gfx_glow(c, gx, gy, 5, COLOR_AMBER, 140);
    }
}

/** 拍摄动画：快门帘、定格、飞进计数窗（有成片时飞走的是成片） */
static void draw_m6_shot(film_app_t *app, gfx_canvas_t *c)
{
    const cam_state_t *cam = &app->cam;
    const uint16_t *photo = cam->dev_ready ? cam->dev_print : (cam->frozen_w ? cam->frozen : NULL);
    if (cam->shot != SHOT_FLY || !photo) {
        return;
    }
    const float t = cam_progress(app->now, cam->shot_t0, SHOT_FLY_MS);
    const float e = t * t;
    const float w = 480.0f + (34.0f - 480.0f) * e;
    const float h = 360.0f + (26.0f - 360.0f) * e;
    const float cx = 240.0f + ((float)M6_COUNTER_X - 240.0f) * e;
    const float cy = 180.0f + ((float)M6_COUNTER_Y - 180.0f) * e;
    const gfx_rect_t dst = gfx_rect((int)(cx - w / 2), (int)(cy - h / 2), (int)w, (int)h);
    const uint8_t opacity = (uint8_t)(255.0f * (1.0f - t * t * t * t));
    gfx_shadow(c, gfx_rect(dst.x, dst.y + 4, dst.w, dst.h), 0, 10, COLOR_BLACK, (uint8_t)(opacity / 3));
    gfx_fill(c, gfx_rect(dst.x - 3, dst.y - 3, dst.w + 6, dst.h + 6), COLOR_PAPER, opacity);
    gfx_blit_scaled(c, photo, FILM_VF_M6_W, FILM_VF_M6_H, FILM_VF_M6_W, gfx_rect(0, 0, FILM_VF_M6_W, FILM_VF_M6_H), dst,
                    opacity);
}

static void render_m6(film_app_t *app, gfx_canvas_t *c)
{
    const cam_state_t *cam = &app->cam;
    const gfx_rect_t vf = gfx_rect(0, 0, SCREEN_W, M6_PLATE_Y);
    if (cam->shot == SHOT_FLASH) {
        gfx_fill(c, vf, COLOR_BLACK, 255);
    } else if ((cam->shot == SHOT_HOLD || cam->shot == SHOT_REVEAL) && cam->frozen_w) {
        const int h = cam->frozen_h > M6_PLATE_Y ? M6_PLATE_Y : cam->frozen_h;
        gfx_copy(c, cam->frozen, cam->frozen_w, h, cam->frozen_w, 0, 0);
        if (cam->shot == SHOT_REVEAL && cam->dev_ready) {
            const float t = cam_ease_in_out(cam_progress(app->now, cam->shot_t0, SHOT_REVEAL_MS));
            gfx_blit_scaled(c, cam->dev_print, FILM_VF_M6_W, FILM_VF_M6_H, FILM_VF_M6_W,
                            gfx_rect(0, 0, FILM_VF_M6_W, h), gfx_rect(0, 0, FILM_VF_M6_W, h), (uint8_t)(255.0f * t));
        }
    } else {
        cam_draw_live(app, c, vf);
    }
    if (cam->shot != SHOT_FLASH) {
        cam_draw_vf_overlay(app, c, vf, true);
    }
    if (cam->shot == SHOT_HOLD || cam->shot == SHOT_REVEAL) {
        draw_developing(app, c, vf);
    }
    if (cam->overlay == CAM_OVL_POPOVER) {
        gfx_dim(c, vf, 184);
    } else if (cam->overlay == CAM_OVL_ROLLEND) {
        gfx_dim(c, vf, 128);
    }

    gfx_blit(c, &img_m6_plate180, 0, M6_PLATE_Y, 255);
    draw_m6_controls(app, c);
    draw_m6_shot(app, c);
    if (cam->overlay == CAM_OVL_POPOVER) {
        cam_draw_popover(app, c, M6_WINDOW_X - POPOVER_CARET_DX);
    } else if (cam->overlay == CAM_OVL_ROLLEND) {
        cam_draw_rollend(app, c);
    }
}

/* ---------------------------------------------------------------- M6 手势 */

static bool in_vf(int y)
{
    return y < M6_PLATE_Y;
}

static bool in_window(int x, int y)
{
    return ui_hit(gfx_rect(M6_WINDOW_X - M6_WINDOW_W / 2, M6_WINDOW_Y - M6_WINDOW_H / 2, M6_WINDOW_W, M6_WINDOW_H), x,
                  y, 6);
}

static void m6_gesture(film_app_t *app, const gesture_t *g)
{
    cam_state_t *cam = &app->cam;
    if (cam_overlay_gesture(app, g, M6_WINDOW_X - POPOVER_CARET_DX)) {
        return;
    }
    switch (g->kind) {
    case GEST_PRESS:
        cam->shutter_down = in_circle(g->x, g->y, M6_SHUTTER_X, M6_SHUTTER_Y, SHUTTER_HIT_R);
        cam->window_down = in_window(g->x, g->y);
        cam->counter_down = in_circle(g->x, g->y, M6_COUNTER_X, M6_COUNTER_Y, COUNTER_HIT_R);
        break;
    case GEST_LONG:
        if (cam->window_down) {
            cam_open(app, CAM_OVL_POPOVER);
            app_feedback(app, FILM_FEEDBACK_CLICK);
        }
        break;
    case GEST_TAP:
        if (cam->shutter_down) {
            cam_fire(app);
        } else if (cam->window_down) {
            app_feedback(app, FILM_FEEDBACK_CLICK);
            app_open_film(app, SCR_CAMERA, app->settings.film);
        } else if (cam->counter_down) {
            app->album.selecting = false;
            app->album.scroll = 0;
            app_go(app, SCR_ALBUM);
        }
        break;
    case GEST_DOUBLE_TAP:
        if (in_vf(g->y)) {
            cam_set_ev(app, 0.0f);
        }
        break;
    case GEST_DRAG_BEGIN:
        cam->shutter_down = false;
        cam->counter_down = false;
        if (g->horizontal && (in_vf(g->y0) || cam->window_down)) {
            cam->dragging_film = true;
            cam->film_drag_start = app->settings.film;
        } else if (!g->horizontal && in_vf(g->y0)) {
            cam->dragging_ev = true;
            cam->ev_drag_start = cam->ev;
        }
        cam->window_down = false;
        break;
    case GEST_DRAG:
        if (cam->dragging_film) {
            app_select_film(app, cam->film_drag_start - g->du / FILM_SWIPE_PX);
        } else if (cam->dragging_ev) {
            cam_set_ev(app, cam->ev_drag_start - (float)g->dv / EV_PX_PER_STOP);
        }
        break;
    case GEST_DRAG_END:
        cam->dragging_film = false;
        cam->dragging_ev = false;
        break;
    case GEST_RELEASE:
        cam->shutter_down = false;
        cam->window_down = false;
        cam->counter_down = false;
        break;
    }
}

/* ---------------------------------------------------------------- 页面接口 */

static void camera_enter(film_app_t *app)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay != CAM_OVL_DEVELOPING && cam->overlay != CAM_OVL_ROLLEND) {
        cam->overlay = CAM_OVL_NONE;
    }
    cam->shutter_down = cam->window_down = cam->counter_down = false;
    cam->dragging_ev = cam->dragging_film = false;
    cam->film_pos = (float)app->settings.film;
    cam_picker_reset(app);
    app_update_preview(app);
    cam_sx_refresh_stack(app);
}

static bool camera_step(film_app_t *app, uint32_t dt)
{
    cam_state_t *cam = &app->cam;
    bool anim = false;

    /* 胶卷窗里的胶卷名追随选中的胶卷 */
    const float d = film_delta(cam->film_pos, (float)app->settings.film);
    if (fabsf(d) > 0.002f) {
        cam->film_pos += d * fminf(1.0f, (float)dt / FILM_ANIM_MS);
        anim = true;
    } else {
        cam->film_pos = (float)app->settings.film;
    }

    /*
     * 拍摄动画的阶段推进（M6）：
     *   FLASH → HOLD（定格原片 + 进度）→ 小样到了 → REVEAL（原片淡到小样，进度条继续走）
     *   → 全尺寸冲完（进度 DONE，取景已恢复）且小样看够 → FLY 飞进计数窗
     * 等待进度时不逐帧重绘：进度事件自己会让界面重画。
     */
    if (cam->shot != SHOT_IDLE) {
        const uint32_t el = app->now - cam->shot_t0;
        const bool done = cam->dev_progress >= FILM_PROGRESS_DONE;
        switch (cam->shot) {
        case SHOT_FLASH:
            anim = true;
            break;
        case SHOT_HOLD:
            anim = !cam->dev_progress;  /* 呼吸灯 */
            break;
        case SHOT_REVEAL:
            anim = el < SHOT_REVEAL_MS + 40;
            break;
        default:
            anim = true;
            break;
        }
        if (cam->shot == SHOT_FLASH && el >= SHOT_FLASH_MS) {
            cam->shot = app->settings.instant ? SHOT_IDLE : SHOT_HOLD;
            cam->shot_t0 = app->now;
        } else if (cam->shot == SHOT_HOLD && el >= SHOT_HOLD_MS && cam->dev_ready) {
            cam->shot = SHOT_REVEAL;
            cam->shot_t0 = app->now;
            anim = true;
        } else if ((cam->shot == SHOT_REVEAL && done && el >= SHOT_REVEAL_MS + SHOT_REVIEW_MS) ||
                   ((cam->shot == SHOT_HOLD || cam->shot == SHOT_REVEAL) && el >= SHOT_STUCK_MS)) {
            cam->shot = SHOT_FLY;
            cam->shot_t0 = app->now;
            anim = true;
        } else if (cam->shot == SHOT_FLY && el >= SHOT_FLY_MS) {
            cam->shot = SHOT_IDLE;
            if (cam->shot_pending_rollend) {
                cam->shot_pending_rollend = false;
                cam_open(app, CAM_OVL_ROLLEND);
            }
        }
    }

    /* 皮革合拢：中点换机身 */
    if (cam->skin_swapping) {
        anim = true;
        const uint32_t el = app->now - cam->skin_t0;
        if (!cam->skin_swapped && el >= SKIN_SWAP_MS) {
            cam->skin_swapped = true;
            app->settings.instant = cam->skin_target_instant;
            app_save_settings(app);
            app_update_preview(app);
            cam_sx_refresh_stack(app);
        }
        if (el >= 2 * SKIN_SWAP_MS) {
            cam->skin_swapping = false;
        }
    }
    if (cam->dragging_ev || app->now < cam->ev_scale_until + 50) {
        anim = true;
    }
    if (cam_sx_step(app)) {
        anim = true;
    }
    if (cam_picker_step(app, dt)) {
        anim = true;
    }
    return anim;
}

static void camera_render(film_app_t *app, gfx_canvas_t *c)
{
    /* 机身选择面板拉开时，被面板盖住的顶部几行不画机身 */
    const int covered = cam_picker_covered_rows(&app->cam);
    const gfx_rect_t clip = gfx_clip_push(c, gfx_rect(0, covered, SCREEN_W, SCREEN_H - covered));
    if (app->settings.instant) {
        cam_sx_render(app, c);
    } else {
        render_m6(app, c);
    }
    cam_draw_skin_swap(app, c);
    gfx_clip_pop(c, clip);
    cam_picker_render(app, c);
}

static void camera_gesture(film_app_t *app, const gesture_t *g)
{
    if (app->cam.skin_swapping || cam_picker_gesture(app, g)) {
        return;
    }
    if (app->settings.instant) {
        cam_sx_gesture(app, g);
    } else {
        m6_gesture(app, g);
    }
}

/**
 * 快门键（M6 与 SX-70 共用）：
 *   - 按下即拍，快门钮同时显示按下态，松开复位；
 *   - 长按菜单、机身选择开着时，第一下只把它们收起回到取景；
 *   - 一卷拍完、SX-70 显影中、M6 冲洗中、换机身动画中都不响应（触摸同样拍不了）。
 */
static void camera_key(film_app_t *app, film_key_t key, bool pressed)
{
    cam_state_t *cam = &app->cam;
    if (key != FILM_KEY_SHUTTER) {
        return;
    }
    if (!pressed) {
        cam->shutter_down = false;
        return;
    }
    if (cam->skin_swapping || cam->picker_dragging) {
        return;
    }
    switch (cam->overlay) {
    case CAM_OVL_NONE:
        break;
    case CAM_OVL_PICKER:
        cam->picker_open = false;   /* 面板收完后 cam_picker_step 会关掉弹出层 */
        return;
    case CAM_OVL_POPOVER:
        cam_close(app);
        app_feedback(app, FILM_FEEDBACK_CLICK);
        return;
    case CAM_OVL_ROLLEND:
    case CAM_OVL_DEVELOPING:
    default:
        return;
    }
    if (cam->shot != SHOT_IDLE) {
        return;
    }
    cam->shutter_down = true;
    cam_fire(app);
}

/**
 * M6：暗房送回小样 → 转回取景方向备用；进度 → 更新进度条；
 * 冲洗失败 → 结束拍摄动画（提示由 app.c 统一给）
 */
static void m6_event(film_app_t *app, const film_event_t *ev)
{
    cam_state_t *cam = &app->cam;
    if (app->settings.instant || cam->shot == SHOT_IDLE || cam->shot == SHOT_FLY || ev->photo_id != cam->dev_id) {
        return;
    }
    if (ev->type == FILM_EVT_DEVELOPED && !ev->preview && ev->screen && ev->width && ev->height) {
        print_to_view(ev->screen, ev->width, ev->height, cam->shot_rot, cam->dev_print);
        cam->dev_w = FILM_VF_M6_W;
        cam->dev_h = FILM_VF_M6_H;
        cam->dev_ready = true;
    } else if (ev->type == FILM_EVT_PROGRESS) {
        if (ev->progress > cam->dev_progress) {
            cam->dev_progress = ev->progress > FILM_PROGRESS_DONE ? FILM_PROGRESS_DONE : ev->progress;
        }
    } else if (ev->type == FILM_EVT_SHOT_FAILED) {
        cam->shot = SHOT_IDLE;
        cam->shot_pending_rollend = false;
    }
}

static void camera_event(film_app_t *app, const film_event_t *ev)
{
    cam_sx_event(app, ev);
    m6_event(app, ev);
}

/** 取景帧所在区域：M6 是铝板以上，SX-70 是机身中间的方框 */
static gfx_rect_t camera_live_rect(const film_app_t *app)
{
    if (app->settings.instant) {
        return gfx_rect(SX_VF_X, 0, FILM_VF_SX_W, FILM_VF_SX_H);
    }
    return gfx_rect(0, 0, SCREEN_W, M6_PLATE_Y);
}

const screen_ops_t g_screen_camera = {
    .enter = camera_enter,
    .leave = NULL,
    .step = camera_step,
    .render = camera_render,
    .gesture = camera_gesture,
    .event = camera_event,
    .key = camera_key,
    .live_rect = camera_live_rect,
};

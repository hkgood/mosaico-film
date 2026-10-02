/*
 * 暗房 · 大图：上方 480×360 照片（竖拍与宝丽来居中显示），左右滑动切换，
 * 底栏显示胶卷与时间，提供删除（二次确认）、重新冲洗、发送到手机。
 */
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "app_private.h"

#define PHOTO_H          360
#define SWIPE_SWITCH_PX  70
#define SLIDE_MS         60.0f
#define META_Y           379     /*!< 胶卷 · 日期 · 时间 一行（照片区与按钮行之间） */

/* 底栏：删除 | 重新冲洗 | 发送到手机，两端收进圆角安全区 */
#define TRASH_W          48
#define SEND_W           200
static const gfx_rect_t s_trash = { BOTTOM_ROW_LEFT, BOTTOM_ROW_Y, TRASH_W, BOTTOM_ROW_H };
static const gfx_rect_t s_redevelop = { BOTTOM_ROW_LEFT + TRASH_W + BOTTOM_ROW_GAP, BOTTOM_ROW_Y,
                                        BOTTOM_ROW_RIGHT - SEND_W - BOTTOM_ROW_GAP -
                                            (BOTTOM_ROW_LEFT + TRASH_W + BOTTOM_ROW_GAP),
                                        BOTTOM_ROW_H };
static const gfx_rect_t s_send = { BOTTOM_ROW_RIGHT - SEND_W, BOTTOM_ROW_Y, SEND_W, BOTTOM_ROW_H };

static const film_photo_t *current(film_app_t *app)
{
    return film_library_get(app->library, app->detail.index);
}

static void load_current(film_app_t *app)
{
    detail_state_t *d = &app->detail;
    const film_photo_t *p = current(app);
    if (!p || p->id == d->loaded_id) {
        return;
    }
    d->loaded_id = p->id;
    d->load_failed = film_library_load_pixels(app->library, p->id, FILM_FILE_SCREEN, d->pixels,
                                              (size_t)FILM_VF_M6_W * FILM_VF_M6_H, &d->w, &d->h) != ESP_OK;
}

static void format_meta(const film_photo_t *p, char *buf, size_t len)
{
    const film_info_t *info = film_info(p->film);
    const time_t t = (time_t)p->time;
    struct tm tm;
    if (p->time > 0 && localtime_r(&t, &tm)) {
        snprintf(buf, len, "%s  ·  %04d.%02d.%02d  ·  %02d:%02d", info->name, tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(buf, len, "%s  ·  ROLL %02u  ·  #%02u", info->name, (unsigned)p->roll, (unsigned)p->frame);
    }
}

static void detail_render(film_app_t *app, gfx_canvas_t *c)
{
    detail_state_t *d = &app->detail;
    gfx_fill(c, gfx_rect(0, 0, SCREEN_W, PHOTO_H), COLOR_INK, 255);
    const film_photo_t *p = current(app);
    const int offset = d->drag_offset + (int)lroundf(d->slide);
    if (p && !d->load_failed && d->w && d->h) {
        /* 按高度 360 等比放进照片区 */
        const int h = PHOTO_H;
        const int w = d->w * h / d->h > SCREEN_W ? SCREEN_W : d->w * h / d->h;
        const gfx_rect_t dst = gfx_rect((SCREEN_W - w) / 2 + offset, 0, w, h);
        gfx_blit_scaled(c, d->pixels, d->w, d->h, d->w, gfx_rect(0, 0, d->w, d->h), dst, 255);
    } else if (p) {
        const gfx_text_style_t st = ui_cjk(&font_noto_13, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &st, 240 + offset, 180, "照片读取失败");
    }
    ui_top_shade(c, HEADER_H + 20, 140);
    ui_icon_button(c, HEADER_BACK_X, HEADER_BACK_Y, &img_icon_back, false);
    char pos[24];
    snprintf(pos, sizeof(pos), "%u / %u", (unsigned)(d->index + 1), (unsigned)film_library_count(app->library));
    const gfx_text_style_t st = ui_style(&font_jost_m14, UI_TEXT_LINK, 0.12f, COLOR_CREAM, 255, GFX_ALIGN_RIGHT,
                                         GFX_ROT_0);
    gfx_text(c, &st, HEADER_RIGHT_X, HEADER_RIGHT_Y, pos);

    gfx_tile(c, &img_tex_vulc, gfx_rect(0, PHOTO_H, SCREEN_W, SCREEN_H - PHOTO_H), 0, 0, 0);
    gfx_fill(c, gfx_rect(0, PHOTO_H, SCREEN_W, 1), COLOR_WHITE, 26);
    if (p) {
        char meta[80];
        format_meta(p, meta, sizeof(meta));
        const gfx_text_style_t ms =
            ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.2f, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
        gfx_text(c, &ms, BOTTOM_ROW_LEFT + 4, META_Y, meta);
    }
    ui_button_line(c, s_trash, NULL, &img_icon_trash, false, COLOR_CREAM, 71);
    ui_button_line(c, s_redevelop, "REDEVELOP", NULL, false, COLOR_CREAM, 71);
    ui_button_primary(c, s_send, "SEND TO PHONE", &img_icon_phone, false, true);

    if (d->confirm_delete) {
        ui_delete_confirm(c, "删除这张照片？");
    }
}

static void switch_photo(film_app_t *app, int delta)
{
    detail_state_t *d = &app->detail;
    const size_t n = film_library_count(app->library);
    const int next = (int)d->index + delta;
    if (next < 0 || (size_t)next >= n) {
        return;
    }
    d->index = (size_t)next;
    d->slide = (float)(delta * 160);
    load_current(app);
    app_feedback(app, FILM_FEEDBACK_DETENT);
}

static void delete_current(film_app_t *app)
{
    detail_state_t *d = &app->detail;
    const film_photo_t *p = current(app);
    d->confirm_delete = false;
    if (!p) {
        return;
    }
    if (film_library_remove(app->library, p->id) != ESP_OK) {
        app_toast(app, "删除失败");
    }
    d->loaded_id = 0;
    const size_t n = film_library_count(app->library);
    if (n == 0) {
        app_go(app, SCR_ALBUM);
        return;
    }
    if (d->index >= n) {
        d->index = n - 1;
    }
    load_current(app);
    app_toast(app, "已删除");
}

static void detail_gesture(film_app_t *app, const gesture_t *g)
{
    detail_state_t *d = &app->detail;
    if (d->confirm_delete) {
        if (g->kind == GEST_TAP) {
            if (ui_delete_confirm_hit(g->x, g->y)) {
                delete_current(app);
            } else {
                d->confirm_delete = false;
            }
        }
        return;
    }
    const film_photo_t *p = current(app);
    switch (g->kind) {
    case GEST_TAP:
        if (ui_back_hit(g->x, g->y)) {
            app_go(app, SCR_ALBUM);
        } else if (p && ui_hit(s_trash, g->x, g->y, 4)) {
            d->confirm_delete = true;
        } else if (p && ui_hit(s_redevelop, g->x, g->y, 4)) {
            app_open_redevelop(app, p->id);
        } else if (p && ui_hit(s_send, g->x, g->y, 4)) {
            app_share(app, &p->id, 1, SCR_DETAIL);
        } else {
            return;
        }
        app_feedback(app, FILM_FEEDBACK_CLICK);
        break;
    case GEST_DRAG:
        if (g->horizontal && g->y0 < PHOTO_H) {
            d->drag_offset = g->dx;
        }
        break;
    case GEST_DRAG_END:
        if (d->drag_offset <= -SWIPE_SWITCH_PX) {
            d->drag_offset = 0;
            switch_photo(app, 1);
        } else if (d->drag_offset >= SWIPE_SWITCH_PX) {
            d->drag_offset = 0;
            switch_photo(app, -1);
        } else {
            d->slide = (float)d->drag_offset;
            d->drag_offset = 0;
        }
        break;
    default:
        break;
    }
}

static bool detail_step(film_app_t *app, uint32_t dt)
{
    detail_state_t *d = &app->detail;
    if (fabsf(d->slide) < 0.5f) {
        d->slide = 0;
        return false;
    }
    d->slide -= d->slide * fminf(1.0f, (float)dt / SLIDE_MS);
    return true;
}

static void detail_enter(film_app_t *app)
{
    detail_state_t *d = &app->detail;
    d->confirm_delete = false;
    d->drag_offset = 0;
    d->slide = 0;
    d->loaded_id = 0;
    const size_t n = film_library_count(app->library);
    if (n && d->index >= n) {
        d->index = n - 1;
    }
    load_current(app);
}

static void detail_event(film_app_t *app, const film_event_t *ev)
{
    /* 新照片插在最前面，当前照片的序号随之后移 */
    size_t index;
    if (ev->type == FILM_EVT_SAVED && app->detail.loaded_id &&
        film_library_find(app->library, app->detail.loaded_id, &index)) {
        app->detail.index = index;
    }
}

const screen_ops_t g_screen_detail = {
    .enter = detail_enter,
    .step = detail_step,
    .render = detail_render,
    .gesture = detail_gesture,
    .event = detail_event,
    .key = app_key_back_to_camera,
};

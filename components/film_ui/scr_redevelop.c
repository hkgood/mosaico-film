/*
 * 暗房 · 重新冲洗：用另一卷胶卷（或换成宝丽来相纸）重新冲洗原片，结果另存为新照片，原片保留。
 *
 * 预览流程：进入时向平台要一张未冲洗的原片预览，然后每当滚筒停稳就要一张当前胶卷的冲洗预览。
 * 平台同一时间只处理一个请求，忙时下一帧重试。按住 HOLD · ORIGINAL 对比原片。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_private.h"

#define VIEW_H          300
#define PLATE_Y         300
/* 铝板从上到下：刻字 → 胶卷说明 → 指针 → 滚筒 → 底行（INSTANT 拨杆 | DEVELOP），底行收进下方圆弧以内 */
#define PLATE_HEAD_Y    (PLATE_Y + 16)
#define PLATE_TAG_Y     (PLATE_Y + 32)
#define PLATE_MARK_Y    (PLATE_Y + 46)
#define DRUM_Y          380
#define DRUM_HALF_H     31
#define DRUM_PITCH      120.0f
#define DRUM_ANIM_MS    90.0f
#define SETTLE_MS       160
#define BOTTOM_CY       436     /*!< 底行的竖直中心 */
#define LEVER_X         150
#define LEVER_LABEL_X   (LEVER_X - 50)  /*!< INSTANT 刻字中心（在拨杆左边，不压住拨杆） */

static const gfx_rect_t s_hold = { 290, 24, 140, 32 };
static const gfx_rect_t s_develop = { 296, BOTTOM_CY - 20, 136, 40 };
static const gfx_rect_t s_lever_hit = { 56, BOTTOM_CY - 22, 124, 44 };

static const film_photo_t *source(film_app_t *app)
{
    return film_library_find(app->library, app->redev.source_id, NULL);
}

static bool result_matches(const redev_state_t *r)
{
    return r->res_film == r->target_film && r->res_instant == r->instant;
}

/** 构造一次重新冲洗请求（预览或正式） */
static film_shot_t make_request(film_app_t *app, const film_photo_t *src, int film, bool preview)
{
    const redev_state_t *r = &app->redev;
    return (film_shot_t) {
        .photo_id = preview ? src->id : 0,
        .source_id = src->id,
        .film = (film_id_t)film,
        .exposure_ev = 0.0f,
        .instant = r->instant,
        .date_stamp = (src->flags & FILM_PHOTO_DATE) != 0,
        .rot = (gfx_rot_t)src->rot,
        .seed = app_random(app),
        .light_leak = false,
        .roll = src->roll,
        .frame = src->frame,
        .time = src->time,
        .balance = film_photo_balance(src),
        .preview_only = preview,
    };
}

/** 按需发出下一个预览请求：先要原片，再要当前胶卷 */
static void pump_requests(film_app_t *app)
{
    redev_state_t *r = &app->redev;
    const film_photo_t *src = source(app);
    if (!src || r->request_in_flight || r->developing || !app->port->redevelop) {
        return;
    }
    int film;
    if (!r->orig_ready) {
        film = FILM_ID_COUNT;
    } else if (!result_matches(r) && app->now >= r->settle_at) {
        film = r->target_film;
    } else {
        return;
    }
    const film_shot_t req = make_request(app, src, film, true);
    const esp_err_t err = app->port->redevelop(app->port->ctx, &req);
    if (err == ESP_OK) {
        r->request_in_flight = true;
        r->want_raw = film == FILM_ID_COUNT;
    } else if (err != ESP_ERR_INVALID_STATE) {
        /* 不是忙，而是真的失败（原片丢失等）：停止请求，避免每帧重试 */
        r->orig_ready = true;
        r->res_film = r->target_film;
        r->res_instant = r->instant;
        app_toast_error(app, err);
    }
}

static void select_film(film_app_t *app, int film)
{
    redev_state_t *r = &app->redev;
    film = film_wrap(film);
    if (film != r->target_film) {
        r->target_film = film;
        r->settle_at = app->now + SETTLE_MS;
        app_feedback(app, FILM_FEEDBACK_DETENT);
    }
}

/* ---------------------------------------------------------------- 绘制 */

/** 把一张屏幕图等比放进 dst（不裁切） */
static void blit_fit(gfx_canvas_t *c, const uint16_t *px, int w, int h, gfx_rect_t dst)
{
    int dw = dst.w;
    int dh = h * dst.w / w;
    if (dh > dst.h) {
        dh = dst.h;
        dw = w * dst.h / h;
    }
    gfx_blit_scaled(c, px, w, h, w, gfx_rect(0, 0, w, h),
                    gfx_rect(dst.x + (dst.w - dw) / 2, dst.y + (dst.h - dh) / 2, dw, dh), 255);
}

static void draw_view(film_app_t *app, gfx_canvas_t *c)
{
    redev_state_t *r = &app->redev;
    const gfx_rect_t view = gfx_rect(0, 0, SCREEN_W, VIEW_H);
    gfx_fill(c, view, COLOR_INK, 255);
    const bool show_original = r->holding_original || !r->res_w;
    if (show_original && r->orig_ready && r->orig_w) {
        blit_fit(c, r->original, r->orig_w, r->orig_h, view);
    } else if (!show_original) {
        blit_fit(c, r->result, r->res_w, r->res_h, view);
    }
    /* 预览还没跟上滚筒时，用暗房状态卡明确反馈，避免小字沉到照片底部。 */
    const bool busy = !r->holding_original && (!result_matches(r) || r->request_in_flight);
    if (busy) {
        const float t = (float)(app->now % 1200) / 1200.0f;
        const gfx_rect_t card = gfx_rect(104, 194, 272, 92);
        gfx_shadow(c, gfx_rect(card.x, card.y + 5, card.w, card.h), 18, 14, COLOR_BLACK, 150);
        gfx_fill_round(c, card, 18, 0x090908, 235);
        gfx_stroke_round(c, card, 18, 1, COLOR_WHITE, 28);
        const gfx_text_style_t title =
            ui_style(&font_jost_m12, 12, 0.26f, COLOR_CREAM, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &title, 240, 222, "DEVELOPING");
        const gfx_text_style_t sub = ui_cjk(&font_noto_11, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &sub, 240, 244, "正在生成新的胶片版本");
        gfx_fill_round(c, gfx_rect(140, 260, 200, 4), 2, 0xFFFAEC, 48);
        const int scan = (int)lroundf(t * 170.0f);
        gfx_glow(c, 140 + scan, 262, 7, COLOR_AMBER, 105);
        gfx_fill_round(c, gfx_rect(140 + scan, 260, 30, 4), 2, COLOR_AMBER, 255);
    }
    ui_top_shade(c, HEADER_H + 16, 153);
    ui_icon_button(c, HEADER_BACK_X, HEADER_BACK_Y, &img_icon_close, false);
    const gfx_text_style_t title = ui_style(&font_jost_m12, 11.5f, 0.22f, COLOR_CREAM, 255, GFX_ALIGN_LEFT,
                                            GFX_ROT_0);
    gfx_text(c, &title, HEADER_TEXT_X, HEADER_TITLE_Y, "REDEVELOP");
    const gfx_text_style_t sub = ui_cjk(&font_noto_11, COLOR_CREAM, 178, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &sub, HEADER_TEXT_X, HEADER_SUB_Y, "另存为新照片，原片保留");
    gfx_fill_round(c, s_hold, 16, 0x0A0A09, r->hold_down ? 200 : 115);
    ui_button_line(c, s_hold, "HOLD · ORIGINAL", NULL, r->hold_down, COLOR_CREAM, 89);
}

static void draw_drum(film_app_t *app, gfx_canvas_t *c)
{
    redev_state_t *r = &app->redev;
    const int wy = DRUM_Y - DRUM_HALF_H;
    ui_sprite(c, &img_win440_base, IMG_WIN440_BASE_OX, IMG_WIN440_BASE_OY, 20, wy, 255);
    const gfx_rect_t inner = gfx_rect(33, wy, 414, 2 * DRUM_HALF_H);
    const gfx_rect_t saved = gfx_clip_push(c, inner);
    const int base = (int)floorf(r->film_pos + 0.5f);
    for (int k = -3; k <= 3; ++k) {
        const float d = (float)(base + k) - r->film_pos;
        const bool sel = fabsf(d) < 0.5f;
        const int x = 240 + (int)lroundf(d * DRUM_PITCH);
        const float edge = fminf((float)(x - inner.x), (float)(inner.x + inner.w - x)) / 100.0f;
        const float fade = edge < 0 ? 0 : (edge > 1 ? 1 : edge);
        const gfx_text_style_t st = ui_style(sel ? &font_jost_m22 : &font_jost_m12, sel ? 22 : 12, 0.08f,
                                             sel ? 0xF6F1E4 : COLOR_CREAM, (uint8_t)(fade * (sel ? 255.0f : 97.0f)),
                                             GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &st, x, DRUM_Y, film_info(base + k)->name);
    }
    gfx_clip_pop(c, saved);
    ui_sprite(c, &img_win440_over, IMG_WIN440_OVER_OX, IMG_WIN440_OVER_OY, 20, wy, 255);
}

static void redev_render(film_app_t *app, gfx_canvas_t *c)
{
    redev_state_t *r = &app->redev;
    draw_view(app, c);
    gfx_blit(c, &img_m6_plate180, 0, PLATE_Y, 255);
    const film_info_t *info = film_info(r->target_film);
    char head[32];
    snprintf(head, sizeof(head), "FILM  ·  %s", info->iso);
    ui_engrave(c, 240, PLATE_HEAD_Y, head, false, GFX_ROT_0);
    const gfx_text_style_t tag = ui_style(&font_jost_r12, 12, 0.01f, COLOR_TAGLINE, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &tag, 240, PLATE_TAG_Y, info->tagline);
    ui_index_mark(c, 240, PLATE_MARK_Y, GFX_ROT_0);
    draw_drum(app, c);
    ui_engrave(c, LEVER_LABEL_X, BOTTOM_CY + 4, "INSTANT", false, GFX_ROT_0);
    ui_lever(c, LEVER_X, BOTTOM_CY, r->instant);
    ui_button_dark(c, s_develop, "DEVELOP", false, !r->developing && source(app) != NULL);

    if (r->developing) {
        gfx_fill(c, gfx_rect(0, 0, SCREEN_W, SCREEN_H), COLOR_BLACK, 150);
        const gfx_text_style_t st = ui_style(&font_jost_m11, 10.5f, 0.42f, COLOR_CREAM, 255, GFX_ALIGN_CENTER,
                                             GFX_ROT_0);
        gfx_text(c, &st, 240, 236, "DEVELOPING");
        const float t = (float)(app->now % 1400) / 1400.0f;
        gfx_fill_round(c, gfx_rect(180, 252, 120, 2), 1, COLOR_WHITE, 40);
        gfx_fill_round(c, gfx_rect(180 + (int)(t * 90.0f), 252, 30, 2), 1, COLOR_AMBER, 255);
    }
}

/* ---------------------------------------------------------------- 交互 */

static void start_develop(film_app_t *app)
{
    redev_state_t *r = &app->redev;
    const film_photo_t *src = source(app);
    if (!src || r->developing || !app->port->redevelop) {
        return;
    }
    if (!app->storage_ok) {
        app_toast(app, "存储不可用，无法保存照片");
        return;
    }
    film_shot_t req = make_request(app, src, r->target_film, false);
    req.photo_id = film_library_reserve_id(app->library);
    const esp_err_t err = app->port->redevelop(app->port->ctx, &req);
    if (err != ESP_OK) {
        app_toast_error(app, err);
        return;
    }
    r->developing = true;
    r->new_id = req.photo_id;
    app_feedback(app, FILM_FEEDBACK_SHUTTER);
}

static void redev_gesture(film_app_t *app, const gesture_t *g)
{
    redev_state_t *r = &app->redev;
    if (r->developing) {
        return;
    }
    switch (g->kind) {
    case GEST_PRESS:
        r->hold_down = ui_hit(s_hold, g->x, g->y, 6);
        r->holding_original = r->hold_down;
        break;
    case GEST_RELEASE:
        r->hold_down = false;
        r->holding_original = false;
        break;
    case GEST_TAP:
        if (ui_back_hit(g->x, g->y)) {
            app_go(app, SCR_DETAIL);
        } else if (ui_hit(s_develop, g->x, g->y, 4)) {
            start_develop(app);
        } else if (ui_hit(s_lever_hit, g->x, g->y, 0)) {
            r->instant = !r->instant;
            r->settle_at = app->now;
            app_feedback(app, FILM_FEEDBACK_LEVER);
        } else if (g->y > DRUM_Y - DRUM_HALF_H && g->y < DRUM_Y + DRUM_HALF_H) {
            const int d = (int)lroundf((float)(g->x - 240) / DRUM_PITCH);
            if (d) {
                select_film(app, r->target_film + d);
            }
        }
        break;
    case GEST_DRAG_BEGIN:
        r->hold_down = r->holding_original;
        if (g->horizontal && !r->holding_original) {
            r->dragging = true;
            r->drag_start_pos = r->film_pos;
        }
        break;
    case GEST_DRAG:
        if (r->dragging) {
            r->film_pos = r->drag_start_pos - (float)g->dx / DRUM_PITCH;
            select_film(app, (int)floorf(r->film_pos + 0.5f));
        }
        break;
    case GEST_DRAG_END:
        if (r->dragging) {
            const float fling = -g->vx / (DRUM_PITCH * 6.0f);
            const float clamped = fling > 2.0f ? 2.0f : (fling < -2.0f ? -2.0f : fling);
            select_film(app, (int)floorf(r->film_pos + clamped + 0.5f));
        }
        r->dragging = false;
        break;
    default:
        break;
    }
}

/** 快门键回取景；正式冲洗中与返回键一样不响应，免得新照片冲到一半离开 */
static void redev_key(film_app_t *app, film_key_t key, bool pressed)
{
    if (!app->redev.developing) {
        app_key_back_to_camera(app, key, pressed);
    }
}

static bool redev_step(film_app_t *app, uint32_t dt)
{
    redev_state_t *r = &app->redev;
    bool anim = r->developing || r->request_in_flight || !result_matches(r);
    if (!r->dragging) {
        float d = fmodf((float)r->target_film - r->film_pos, (float)FILM_ID_COUNT);
        if (d > FILM_ID_COUNT / 2.0f) {
            d -= FILM_ID_COUNT;
        } else if (d < -FILM_ID_COUNT / 2.0f) {
            d += FILM_ID_COUNT;
        }
        if (fabsf(d) > 0.002f) {
            r->film_pos += d * fminf(1.0f, (float)dt / DRUM_ANIM_MS);
            anim = true;
        } else {
            r->film_pos = (float)r->target_film;
        }
    }
    pump_requests(app);
    return anim;
}

static void redev_event(film_app_t *app, const film_event_t *ev)
{
    redev_state_t *r = &app->redev;
    if (ev->type == FILM_EVT_DEVELOPED && ev->preview && ev->photo_id == r->source_id && ev->screen) {
        r->request_in_flight = false;
        if ((size_t)ev->width * ev->height > (size_t)FILM_VF_M6_W * FILM_VF_M6_H) {
            return;
        }
        uint16_t *dst = ev->film == FILM_ID_COUNT ? r->original : r->result;
        memcpy(dst, ev->screen, sizeof(uint16_t) * ev->width * ev->height);
        if (ev->film == FILM_ID_COUNT) {
            r->orig_w = ev->width;
            r->orig_h = ev->height;
            r->orig_ready = true;
        } else {
            r->res_w = ev->width;
            r->res_h = ev->height;
            r->res_film = ev->film;
            r->res_instant = ev->instant;
        }
    } else if (ev->type == FILM_EVT_SHOT_FAILED && ev->photo_id == r->source_id) {
        /* 预览失败：放弃这一档，提示已由 app 给出 */
        r->request_in_flight = false;
        if (r->want_raw) {
            r->orig_ready = true;
        }
        r->res_film = r->target_film;
        r->res_instant = r->instant;
    } else if (r->developing && ev->photo_id == r->new_id) {
        if (ev->type == FILM_EVT_SAVED) {
            r->developing = false;
            app_open_detail(app, 0);
            app_toast(app, "已另存为新照片");
        } else if (ev->type == FILM_EVT_SHOT_FAILED) {
            r->developing = false;
        }
    }
}

static void redev_enter(film_app_t *app)
{
    redev_state_t *r = &app->redev;
    const film_photo_t *src = source(app);
    r->instant = src && (src->flags & FILM_PHOTO_INSTANT);
    r->target_film = src && src->film < FILM_ID_COUNT ? src->film : 0;
    r->film_pos = (float)r->target_film;
    /* 滚筒停在原片胶卷上；拿到原片预览后再请求这卷胶卷的冲洗预览 */
    r->res_film = -1;
    r->res_instant = r->instant;
    r->res_w = r->res_h = 0;
    r->orig_ready = false;
    r->orig_w = r->orig_h = 0;
    r->request_in_flight = false;
    r->developing = false;
    r->holding_original = r->hold_down = r->dragging = false;
    r->settle_at = app->now;
}

const screen_ops_t g_screen_redevelop = {
    .enter = redev_enter,
    .step = redev_step,
    .render = redev_render,
    .gesture = redev_gesture,
    .event = redev_event,
    .key = redev_key,
};

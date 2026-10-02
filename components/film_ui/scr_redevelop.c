/*
 * 暗房 · 重新冲洗：用另一卷胶卷（或换成宝丽来相纸）重新冲洗原片，结果另存为新照片，原片保留。
 *
 * 预览流程：进入时向平台要一张未冲洗的原片预览，然后每当胶卷行停稳就要一张当前胶卷的冲洗预览。
 * 平台同一时间只处理一个请求，忙时下一帧重试。按住 ORIGINAL 对比原片。
 * 胶卷行左右滑动换卷，点一下打开全屏胶卷页；从那里回来时保留预览与选择。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_private.h"

#define VIEW_H          300
#define PLATE_Y         300
/* 铝板从上到下：胶卷行 → 底行（INSTANT 拨杆 | DEVELOP），底行收进下方圆弧以内 */
#define ROW_RADIUS      14
#define ROW_CAN_X       8       /*!< 胶卷行内（相对行左上角）：暗盒、名字、说明、右箭头 */
#define ROW_CAN_Y       6
#define ROW_TEXT_X      78
#define ROW_NAME_V      33
#define ROW_SUB_V       57
#define ROW_CHEVRON_R   30      /*!< 右箭头中心离行右边缘 */
#define ROW_BG_TOP      0x141413U
#define ROW_BG_DOWN     0x1F1E1BU
#define ROW_BG_BOTTOM   0x070707U
#define ROW_SLIDE_PX    90      /*!< 换卷时行内容横移的距离（同时淡入淡出） */
#define FILM_DRAG_PITCH 120.0f  /*!< 横向拖动多少像素换一卷 */
#define FILM_ANIM_MS    90.0f
#define SETTLE_MS       160
#define BOTTOM_CY       436     /*!< 底行的竖直中心 */
#define LEVER_X         150
#define LEVER_LABEL_X   (LEVER_X - 50)  /*!< INSTANT 刻字中心（在拨杆左边，不压住拨杆） */

static const gfx_rect_t s_hold = { 290, 24, 140, 32 };
static const gfx_rect_t s_film_row = { 36, PLATE_Y + 14, 408, 82 };
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
    /* 预览还没跟上胶卷行时，用暗房状态卡明确反馈，避免小字沉到照片底部。 */
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
        const gfx_text_style_t sub = ui_cjk(&font_noto_12, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &sub, 240, 244, "正在生成新的胶片版本");
        gfx_fill_round(c, gfx_rect(140, 260, 200, 4), 2, 0xFFFAEC, 48);
        const int scan = (int)lroundf(t * 170.0f);
        gfx_glow(c, 140 + scan, 262, 7, COLOR_AMBER, 105);
        gfx_fill_round(c, gfx_rect(140 + scan, 260, 30, 4), 2, COLOR_AMBER, 255);
    }
    ui_top_shade(c, HEADER_H + 16, 153);
    ui_icon_button(c, HEADER_BACK_X, HEADER_BACK_Y, &img_icon_close, false);
    const gfx_text_style_t title = ui_style(&font_jost_m12, 12, 0.22f, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &title, HEADER_TEXT_X, HEADER_TITLE_Y, "REDEVELOP");
    const gfx_text_style_t sub = ui_cjk(&font_noto_12, COLOR_CREAM, 178, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &sub, HEADER_TEXT_X, HEADER_SUB_Y, "另存为新照片，原片保留");
    gfx_fill_round(c, s_hold, 16, 0x0A0A09, r->hold_down ? 200 : 115);
    ui_button_line(c, s_hold, "ORIGINAL", NULL, r->hold_down, COLOR_CREAM, 89);
}

/** 胶卷行的一格内容：暗盒 + 名字 + 说明，横移 dx、透明度 alpha */
static void draw_row_item(gfx_canvas_t *c, int film, int dx, uint8_t alpha)
{
    const gfx_rect_t row = s_film_row;
    const film_info_t *info = film_info(film);
    gfx_blit(c, info->canister_small, row.x + ROW_CAN_X + dx, row.y + ROW_CAN_Y, alpha);
    const gfx_text_style_t name =
        ui_style(&font_jost_m22, 22, 0.08f, 0xF6F1E4, alpha, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &name, row.x + ROW_TEXT_X + dx, row.y + ROW_NAME_V, info->name);
    char sub[40];
    snprintf(sub, sizeof(sub), "%s  \xC2\xB7  TAP TO CHANGE", info->iso);
    const gfx_text_style_t st =
        ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.2f, COLOR_MUTED, alpha, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &st, row.x + ROW_TEXT_X + 1 + dx, row.y + ROW_SUB_V, sub);
}

/** 铝板上的胶卷行：深色圆角槽，换卷时内容横移淡入淡出 */
static void draw_film_row(film_app_t *app, gfx_canvas_t *c)
{
    const redev_state_t *r = &app->redev;
    const gfx_rect_t row = s_film_row;
    ui_safe_check(row, ROW_RADIUS, "redevelop film row");
    gfx_shadow(c, gfx_rect(row.x, row.y + 2, row.w, row.h), ROW_RADIUS, 4, COLOR_BLACK, 128);
    gfx_gradient_v(c, row, ROW_RADIUS, r->row_down ? ROW_BG_DOWN : ROW_BG_TOP, 255, ROW_BG_BOTTOM, 255);
    gfx_fill(c, gfx_rect(row.x + ROW_RADIUS, row.y, row.w - 2 * ROW_RADIUS, 1), COLOR_WHITE, 20);

    const gfx_rect_t saved = gfx_clip_push(c, gfx_rect(row.x + 2, row.y + 2, row.w - 4, row.h - 4));
    const int base = (int)floorf(r->film_pos + 0.5f);
    for (int k = -1; k <= 1; ++k) {
        const float d = (float)(base + k) - r->film_pos;
        if (fabsf(d) < 1.0f) {
            draw_row_item(c, base + k, (int)lroundf(d * ROW_SLIDE_PX), (uint8_t)lroundf((1.0f - fabsf(d)) * 255.0f));
        }
    }
    gfx_clip_pop(c, saved);
    gfx_icon(c, &img_icon_back, row.x + row.w - ROW_CHEVRON_R, row.y + row.h / 2, GFX_ROT_180, COLOR_CREAM, 200);
}

static void redev_render(film_app_t *app, gfx_canvas_t *c)
{
    redev_state_t *r = &app->redev;
    draw_view(app, c);
    gfx_blit(c, &img_m6_plate180, 0, PLATE_Y, 255);
    draw_film_row(app, c);
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
        r->row_down = ui_hit(s_film_row, g->x, g->y, 0);
        break;
    case GEST_RELEASE:
        r->hold_down = false;
        r->holding_original = false;
        r->row_down = false;
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
        } else if (ui_hit(s_film_row, g->x, g->y, 0)) {
            app_feedback(app, FILM_FEEDBACK_CLICK);
            r->picking_film = true;
            app_open_film(app, SCR_REDEVELOP, r->target_film);
        }
        break;
    case GEST_DRAG_BEGIN:
        r->hold_down = r->holding_original;
        r->row_down = false;
        if (g->horizontal && !r->holding_original) {
            r->dragging = true;
            r->drag_start_pos = r->film_pos;
        }
        break;
    case GEST_DRAG:
        if (r->dragging) {
            r->film_pos = r->drag_start_pos - (float)g->dx / FILM_DRAG_PITCH;
            select_film(app, (int)floorf(r->film_pos + 0.5f));
        }
        break;
    case GEST_DRAG_END:
        if (r->dragging) {
            const float fling = -g->vx / (FILM_DRAG_PITCH * 6.0f);
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
            r->film_pos += d * fminf(1.0f, (float)dt / FILM_ANIM_MS);
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

void redevelop_set_film(film_app_t *app, int film)
{
    if (film_wrap(film) == app->redev.target_film) {
        app_feedback(app, FILM_FEEDBACK_CLICK);
        return;
    }
    select_film(app, film);
}

static void redev_enter(film_app_t *app)
{
    redev_state_t *r = &app->redev;
    r->holding_original = r->hold_down = r->row_down = r->dragging = false;
    if (r->picking_film) {
        /* 从全屏胶卷页回来：预览、拨杆与选择都保留，胶卷行从原来那卷滑到新选的卷 */
        r->picking_film = false;
        return;
    }
    const film_photo_t *src = source(app);
    r->instant = src && (src->flags & FILM_PHOTO_INSTANT);
    r->target_film = src && src->film < FILM_ID_COUNT ? src->film : 0;
    r->film_pos = (float)r->target_film;
    /* 胶卷行停在原片胶卷上；拿到原片预览后再请求这卷胶卷的冲洗预览 */
    r->res_film = -1;
    r->res_instant = r->instant;
    r->res_w = r->res_h = 0;
    r->orig_ready = false;
    r->orig_w = r->orig_h = 0;
    r->request_in_flight = false;
    r->developing = false;
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

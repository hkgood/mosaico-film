/*
 * 暗房 · 胶卷：全屏三列网格，每款胶卷一张卡片（3D 渲染的 135 暗盒 + 名字 + ISO），上下拖动带惯性。
 *
 * 从取景页（M6 胶卷窗、SX-70 胶片盒、一卷拍完后换胶卷）或重新冲洗页的胶卷行打开，两种机身同一个样式；
 * 点一张卡片即装上这卷并立刻回到来源页，返回键不换卷。页面不显示取景。
 * 打开期间平台事件照常转给来源页：M6 冲洗进度、重新冲洗的预览结果不会因为离开而丢掉。
 */
#include <math.h>
#include <stdio.h>

#include "app_private.h"

/* 布局（屏幕坐标，取自已确认的 v2 样稿） */
#define GRID_TOP        (HEADER_H + 12)
#define CARD_W          142
#define CARD_H          168
#define CARD_RADIUS     14
#define ROW_PITCH       178
#define GRID_BOTTOM_PAD 12      /*!< 滚到底时最后一行下面留的空 */
#define CAN_X           17      /*!< 暗盒图在卡片里的左上角 */
#define CAN_Y           6
#define NAME_V          138     /*!< 卡片里胶卷名的竖直中心 */
#define ISO_V           155     /*!< ISO / LOADED 一行的竖直中心 */
#define LOADED_DOT_R    3
#define LOADED_DOT_GAP  7
#define FADE_TOP        400     /*!< 底部渐隐：提示还能往下滚 */

/* 卡片配色 */
#define CARD_BG         0x1C1B19U
#define CARD_BG_ON      0x23211DU   /*!< 装着的这卷 */
#define CARD_BG_DOWN    0x2A2824U   /*!< 按下 */
#define CARD_LINE       0x34322EU
#define NAME_ON         0xF6F1E4U

static const int s_col_x[3] = { 18, 169, 320 };

static int rows(void)
{
    return (FILM_ID_COUNT + 2) / 3;
}

static float max_scroll(void)
{
    const int content = GRID_TOP + rows() * ROW_PITCH - (ROW_PITCH - CARD_H) + GRID_BOTTOM_PAD;
    return content > SCREEN_H ? (float)(content - SCREEN_H) : 0.0f;
}

static float clamp_scroll(float s)
{
    const float max = max_scroll();
    return s < 0 ? 0 : (s > max ? max : s);
}

static gfx_rect_t card_rect(const film_page_state_t *f, int film)
{
    const int y = GRID_TOP + (film / 3) * ROW_PITCH - (int)lroundf(f->scroll);
    return gfx_rect(s_col_x[film % 3], y, CARD_W, CARD_H);
}

/** 屏幕坐标 → 胶卷编号，不在卡片上返回 -1 */
static int card_at(const film_page_state_t *f, int x, int y)
{
    if (y < HEADER_H) {
        return -1;
    }
    for (int i = 0; i < FILM_ID_COUNT; ++i) {
        if (ui_hit(card_rect(f, i), x, y, 0)) {
            return i;
        }
    }
    return -1;
}

/* ---------------------------------------------------------------- 绘制 */

/** 装着的这卷：琥珀圆点 + LOADED，整体居中 */
static void draw_loaded(gfx_canvas_t *c, int cx, int cy)
{
    const gfx_text_style_t st =
        ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.2f, COLOR_AMBER, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    const int text_w = gfx_text_width_q4(&st, "LOADED") / 16;
    const int left = cx - (2 * LOADED_DOT_R + LOADED_DOT_GAP + text_w) / 2;
    gfx_glow(c, left + LOADED_DOT_R, cy, 6, COLOR_AMBER, 120);
    gfx_circle_q4(c, (left + LOADED_DOT_R) * 16, cy * 16, LOADED_DOT_R * 16, COLOR_AMBER, 255);
    gfx_text(c, &st, left + 2 * LOADED_DOT_R + LOADED_DOT_GAP, cy, "LOADED");
}

static void draw_card(gfx_canvas_t *c, const film_page_state_t *f, int film)
{
    const gfx_rect_t r = card_rect(f, film);
    const bool on = film == f->current;
    const film_info_t *info = film_info(film);
    if (on) {
        gfx_shadow(c, r, CARD_RADIUS, 12, COLOR_AMBER, 46);
    }
    gfx_fill_round(c, r, CARD_RADIUS, film == f->pressed ? CARD_BG_DOWN : (on ? CARD_BG_ON : CARD_BG), 255);
    gfx_stroke_round(c, r, CARD_RADIUS, on ? 2 : 1, on ? COLOR_AMBER : CARD_LINE, 255);
    gfx_blit(c, info->canister, r.x + CAN_X, r.y + CAN_Y, 255);

    const int cx = r.x + CARD_W / 2;
    const gfx_text_style_t name =
        ui_style(&font_jost_m14, UI_TEXT_LINK, 0.08f, on ? NAME_ON : COLOR_CREAM, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &name, cx, r.y + NAME_V, info->name);
    if (on) {
        draw_loaded(c, cx, r.y + ISO_V);
    } else {
        const gfx_text_style_t iso =
            ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.2f, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &iso, cx, r.y + ISO_V, info->iso);
    }
}

static void film_render(film_app_t *app, gfx_canvas_t *c)
{
    const film_page_state_t *f = &app->film;
    gfx_fill(c, gfx_rect(0, 0, SCREEN_W, SCREEN_H), COLOR_INK, 255);
    const gfx_rect_t saved = gfx_clip_push(c, gfx_rect(0, HEADER_H, SCREEN_W, SCREEN_H - HEADER_H));
    for (int i = 0; i < FILM_ID_COUNT; ++i) {
        const gfx_rect_t r = card_rect(f, i);
        if (r.y + r.h > HEADER_H && r.y < SCREEN_H) {
            draw_card(c, f, i);
        }
    }
    if (f->scroll < max_scroll() - 0.5f) {
        gfx_gradient_v(c, gfx_rect(0, FADE_TOP, SCREEN_W, SCREEN_H - FADE_TOP), 0, COLOR_INK, 0, COLOR_INK, 217);
    }
    gfx_clip_pop(c, saved);

    /* 副标题写明是给哪台机身（或重新冲洗）选卷 */
    const char *source = f->return_to == SCR_REDEVELOP ? "REDEVELOP" : (app->settings.instant ? "SX-70" : "M6");
    char sub[40];
    snprintf(sub, sizeof(sub), "%s  \xC2\xB7  %d STOCKS", source, FILM_ID_COUNT);
    ui_header(c, "FILM", sub, NULL, f->back_down);
}

/* ---------------------------------------------------------------- 交互 */

/** 装上 film 并回到来源页 */
static void pick(film_app_t *app, int film)
{
    film_page_state_t *f = &app->film;
    if (f->return_to == SCR_REDEVELOP) {
        redevelop_set_film(app, film);
    } else if (film != app->settings.film) {
        app_select_film(app, film);
    } else {
        app_feedback(app, FILM_FEEDBACK_CLICK);
    }
    app_go(app, f->return_to);
}

static void film_gesture(film_app_t *app, const gesture_t *g)
{
    film_page_state_t *f = &app->film;
    switch (g->kind) {
    case GEST_PRESS:
        f->velocity = 0;
        f->back_down = g->y < HEADER_H && ui_back_hit(g->x, g->y);
        f->pressed = (int8_t)card_at(f, g->x, g->y);
        break;
    case GEST_TAP: {
        /* 页眉整条都是点击区，但只有左边返回键有用（不向下延伸，免得误触第一行卡片） */
        if (g->y < HEADER_H) {
            if (g->x < BACK_HIT_W) {
                app_feedback(app, FILM_FEEDBACK_CLICK);
                app_go(app, f->return_to);
            }
            return;
        }
        const int film = card_at(f, g->x, g->y);
        if (film >= 0) {
            pick(app, film);
        }
        break;
    }
    case GEST_DRAG_BEGIN:
        f->pressed = -1;
        f->back_down = false;
        f->dragging = true;
        f->drag_start_scroll = f->scroll;
        break;
    case GEST_DRAG:
        if (f->dragging) {
            f->scroll = clamp_scroll(f->drag_start_scroll - (float)g->dy);
        }
        break;
    case GEST_DRAG_END:
        f->dragging = false;
        f->velocity = -g->vy;
        break;
    case GEST_RELEASE:
        f->pressed = -1;
        f->back_down = false;
        break;
    default:
        break;
    }
}

static bool film_step(film_app_t *app, uint32_t dt)
{
    film_page_state_t *f = &app->film;
    if (f->dragging || fabsf(f->velocity) < SCROLL_MIN_VELOCITY) {
        f->velocity = f->dragging ? f->velocity : 0.0f;
        return false;
    }
    f->scroll += f->velocity * (float)dt / 1000.0f;
    f->velocity *= expf(-SCROLL_FRICTION * (float)dt);
    const float s = clamp_scroll(f->scroll);
    if (s != f->scroll) {
        f->scroll = s;
        f->velocity = 0;
    }
    return true;
}

/** 来源页还在等的平台事件（冲洗进度、预览结果）照常交给它 */
static void film_event(film_app_t *app, const film_event_t *ev)
{
    const screen_ops_t *from = app->film.return_to == SCR_REDEVELOP ? &g_screen_redevelop : &g_screen_camera;
    if (from->event) {
        from->event(app, ev);
    }
}

static void film_enter(film_app_t *app)
{
    film_page_state_t *f = &app->film;
    f->velocity = 0;
    f->dragging = false;
    f->pressed = -1;
    f->back_down = false;
    /* 滚到能看全装着的这一卷 */
    const int bottom = GRID_TOP + (f->current / 3) * ROW_PITCH + CARD_H + GRID_BOTTOM_PAD;
    f->scroll = clamp_scroll(bottom > SCREEN_H ? (float)(bottom - SCREEN_H) : 0.0f);
}

const screen_ops_t g_screen_film = {
    .enter = film_enter,
    .step = film_step,
    .render = film_render,
    .gesture = film_gesture,
    .event = film_event,
    .key = app_key_back_to_camera,
};

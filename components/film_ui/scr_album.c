/*
 * 暗房 · 相册：三列网格（普通照片裁满格子，宝丽来以小相纸显示），上下拖动带惯性，
 * SELECT 进入多选（最多 20 张），底栏：已选张数 | 删除（二次确认） | SEND TO PHONE。
 */
#include <math.h>
#include <stdio.h>

#include "app_private.h"

#define GRID_TOP        (HEADER_H + 3)
#define CELL_W          158
#define CELL_H          119
#define ROW_PITCH       (CELL_H + 3)
#define SELECT_BAR_Y    (BOTTOM_ROW_Y - 14)
#define SELECT_COUNT_Y  (BOTTOM_ROW_Y + BOTTOM_ROW_H / 2)
#define FRICTION        0.0045f   /*!< 惯性衰减（每毫秒） */
#define MIN_VELOCITY    20.0f

static const int s_col_x[3] = { 0, 161, 322 };
/* 多选底栏：右侧发送按钮与大图页的 SEND TO PHONE 同宽同位，删除按钮紧挨在它左边 */
#define SEND_W   200
#define TRASH_W  48
static const gfx_rect_t s_send = { BOTTOM_ROW_RIGHT - SEND_W, BOTTOM_ROW_Y, SEND_W, BOTTOM_ROW_H };
static const gfx_rect_t s_trash = { BOTTOM_ROW_RIGHT - SEND_W - BOTTOM_ROW_GAP - TRASH_W, BOTTOM_ROW_Y, TRASH_W,
                                    BOTTOM_ROW_H };

static int cell_width(int col)
{
    return col == 2 ? SCREEN_W - s_col_x[2] : CELL_W;
}

static int view_bottom(const album_state_t *a)
{
    return a->selecting ? SELECT_BAR_Y : SCREEN_H;
}

static float max_scroll(film_app_t *app)
{
    const size_t n = film_library_count(app->library);
    const int rows = (int)((n + 2) / 3);
    const float content = (float)(GRID_TOP + rows * ROW_PITCH + 24);
    const float view = (float)view_bottom(&app->album);
    return content > view ? content - view : 0.0f;
}

static bool is_selected(const album_state_t *a, uint32_t id, size_t *ret_pos)
{
    for (size_t i = 0; i < a->n_selected; ++i) {
        if (a->selected[i] == id) {
            if (ret_pos) {
                *ret_pos = i;
            }
            return true;
        }
    }
    return false;
}

static void toggle_selected(film_app_t *app, uint32_t id)
{
    album_state_t *a = &app->album;
    size_t pos;
    if (is_selected(a, id, &pos)) {
        a->selected[pos] = a->selected[--a->n_selected];
    } else if (a->n_selected < SELECT_MAX) {
        a->selected[a->n_selected++] = id;
    } else {
        app_toast(app, "最多选择 20 张");
        return;
    }
    app_feedback(app, FILM_FEEDBACK_CLICK);
}

/** 屏幕坐标 → 照片序号，不在格子上返回 -1 */
static int cell_at(film_app_t *app, int x, int y)
{
    if (y < HEADER_H || y >= view_bottom(&app->album)) {
        return -1;
    }
    const int cy = y - GRID_TOP + (int)lroundf(app->album.scroll);
    if (cy < 0 || cy % ROW_PITCH >= CELL_H) {
        return -1;
    }
    int col = -1;
    for (int i = 0; i < 3; ++i) {
        if (x >= s_col_x[i] && x < s_col_x[i] + cell_width(i)) {
            col = i;
        }
    }
    const int index = (cy / ROW_PITCH) * 3 + col;
    return col >= 0 && index < (int)film_library_count(app->library) ? index : -1;
}

static void draw_cell(film_app_t *app, gfx_canvas_t *c, const film_photo_t *p, gfx_rect_t r)
{
    const thumb_slot_t *t = app_thumb(app, p->id);
    if (p->flags & FILM_PHOTO_INSTANT) {
        gfx_fill(c, r, 0x171716, 255);
        const gfx_rect_t paper = gfx_rect(r.x + (r.w - 86) / 2, r.y + 8, 86, 103);
        gfx_shadow(c, gfx_rect(paper.x, paper.y + 3, paper.w, paper.h), 0, 8, COLOR_BLACK, 150);
        if (t) {
            gfx_blit_scaled(c, t->pixels, t->w, t->h, t->w, gfx_rect(0, 0, t->w, t->h), paper, 255);
        } else {
            ui_paper(c, paper, 0, 0);
            const gfx_rect_t image = gfx_rect(paper.x + 5, paper.y + 5, 76, 76);
            gfx_fill(c, image, 0x1D1D1C, 255);
            gfx_stroke_round(c, gfx_rect(image.x - 1, image.y - 1, image.w + 2, image.h + 2), 0, 1, 0x625B50, 72);
        }
    } else if (t) {
        gfx_blit_cover(c, t->pixels, t->w, t->h, t->w, r, 255);
    } else {
        gfx_fill(c, r, 0x1D1D1C, 255);
    }
}

static void album_render(film_app_t *app, gfx_canvas_t *c)
{
    album_state_t *a = &app->album;
    gfx_fill(c, gfx_rect(0, 0, SCREEN_W, SCREEN_H), COLOR_INK, 255);
    const size_t n = film_library_count(app->library);
    const int bottom = view_bottom(a);
    const gfx_rect_t saved = gfx_clip_push(c, gfx_rect(0, HEADER_H, SCREEN_W, bottom - HEADER_H));
    const int scroll = (int)lroundf(a->scroll);
    const int first_row = scroll > GRID_TOP ? (scroll - GRID_TOP) / ROW_PITCH : 0;
    for (int row = first_row;; ++row) {
        const int y = GRID_TOP + row * ROW_PITCH - scroll;
        if (y >= bottom || (size_t)(row * 3) >= n) {
            break;
        }
        for (int col = 0; col < 3; ++col) {
            const size_t index = (size_t)(row * 3 + col);
            const film_photo_t *p = film_library_get(app->library, index);
            if (!p) {
                break;
            }
            const gfx_rect_t r = gfx_rect(s_col_x[col], y, cell_width(col), CELL_H);
            draw_cell(app, c, p, r);
            if (a->selecting) {
                const bool on = is_selected(a, p->id, NULL);
                if (on) {
                    gfx_stroke_round(c, r, 0, 2, COLOR_AMBER, 255);
                }
                ui_check(c, r.x + r.w - 30, r.y + 8, on);
            }
        }
    }
    if (!a->selecting) {
        gfx_gradient_v(c, gfx_rect(0, 400, SCREEN_W, 80), 0, COLOR_INK, 0, COLOR_INK, 217);
    }
    gfx_clip_pop(c, saved);

    if (n == 0) {
        gfx_ring_q4(c, 240 * 16, 214 * 16, 44 * 16, 20, 0x45433C, 255);
        gfx_ring_q4(c, 240 * 16, 214 * 16, 20 * 16, 16, 0x777166, 220);
        gfx_fill(c, gfx_rect(196, 213, 88, 1), 0x45433C, 160);
        gfx_fill(c, gfx_rect(239, 170, 1, 88), 0x45433C, 160);
        const gfx_text_style_t title =
            ui_style(&font_jost_m14, 14, 0.24f, app->storage_ok ? COLOR_CREAM : COLOR_LED, 255, GFX_ALIGN_CENTER,
                     GFX_ROT_0);
        gfx_text(c, &title, 240, 288, app->storage_ok ? "NO FRAMES YET" : "STORAGE OFFLINE");
        const gfx_text_style_t sub = ui_cjk(&font_noto_11, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &sub, 240, 316,
                 app->storage_ok ? "按下快门，拍摄这一卷的第一张照片" : "存储不可用，暂时无法读取相册");
    }

    char title[24], sub[24];
    if (a->selecting) {
        ui_header(c, "SELECT", "UP TO 20 FRAMES", "CANCEL", false);
        gfx_tile(c, &img_tex_vulc, gfx_rect(0, SELECT_BAR_Y, SCREEN_W, SCREEN_H - SELECT_BAR_Y), 0, 0, 0);
        gfx_fill(c, gfx_rect(0, SELECT_BAR_Y, SCREEN_W, 1), COLOR_WHITE, 20);
        char count[24];
        snprintf(count, sizeof(count), "%u SELECTED", (unsigned)a->n_selected);
        const gfx_text_style_t st = ui_style(&font_jost_m11, 10.5f, 0.22f, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
        gfx_text(c, &st, BOTTOM_ROW_LEFT + 4, SELECT_COUNT_Y, count);
        /* 一张没选时删除按钮变灰，点了也没反应 */
        const bool any = a->n_selected > 0;
        ui_button_line(c, s_trash, NULL, &img_icon_trash, false, any ? COLOR_CREAM : COLOR_MUTED, any ? 71 : 40);
        ui_button_primary(c, s_send, "SEND TO PHONE", &img_icon_phone, false, any);
        if (a->confirm_delete) {
            char question[32];
            snprintf(question, sizeof(question), "删除 %u 张照片？", (unsigned)a->n_selected);
            ui_delete_confirm(c, question);
        }
    } else {
        snprintf(title, sizeof(title), "ROLL %02u", (unsigned)app->settings.roll);
        snprintf(sub, sizeof(sub), "%u FRAMES", (unsigned)n);
        ui_header(c, title, sub, n ? "SELECT" : "", false);
    }
}

static bool album_step(film_app_t *app, uint32_t dt)
{
    album_state_t *a = &app->album;
    if (a->dragging || fabsf(a->velocity) < MIN_VELOCITY) {
        a->velocity = a->dragging ? a->velocity : 0.0f;
        return false;
    }
    a->scroll += a->velocity * (float)dt / 1000.0f;
    a->velocity *= expf(-FRICTION * (float)dt);
    const float max = max_scroll(app);
    if (a->scroll < 0) {
        a->scroll = 0;
        a->velocity = 0;
    } else if (a->scroll > max) {
        a->scroll = max;
        a->velocity = 0;
    }
    return true;
}

/** 删掉已选的照片（索引只写一次），退出多选并提示结果 */
static void delete_selected(film_app_t *app)
{
    album_state_t *a = &app->album;
    a->confirm_delete = false;
    size_t removed = 0;
    const esp_err_t err = film_library_remove_many(app->library, a->selected, a->n_selected, &removed);
    a->selecting = false;
    a->n_selected = 0;
    const float max = max_scroll(app);
    if (a->scroll > max) {
        a->scroll = max;
    }
    if (err != ESP_OK) {
        app_toast(app, "删除失败");
        app_feedback(app, FILM_FEEDBACK_ERROR);
        return;
    }
    char text[24];
    snprintf(text, sizeof(text), "已删除 %u 张", (unsigned)removed);
    app_toast(app, text);
}

static void album_gesture(film_app_t *app, const gesture_t *g)
{
    album_state_t *a = &app->album;
    if (a->confirm_delete) {
        /* 弹窗是模态的：点 DELETE 删除，点其他任何地方取消 */
        if (g->kind == GEST_TAP) {
            if (ui_delete_confirm_hit(g->x, g->y)) {
                delete_selected(app);
            } else {
                a->confirm_delete = false;
            }
            app_feedback(app, FILM_FEEDBACK_CLICK);
        }
        return;
    }
    switch (g->kind) {
    case GEST_PRESS:
        a->velocity = 0;
        break;
    case GEST_TAP: {
        /* 页眉整条都是点击区：左边返回，右边 SELECT / CANCEL（不向下延伸，免得误触第一行照片） */
        if (g->y < HEADER_H) {
            if (g->x < BACK_HIT_W) {
                if (a->selecting) {
                    a->selecting = false;
                } else {
                    app_go(app, SCR_CAMERA);
                }
            } else if (g->x > SCREEN_W - HEADER_RIGHT_HIT_W && film_library_count(app->library)) {
                a->selecting = !a->selecting;
                a->n_selected = 0;
            }
            app_feedback(app, FILM_FEEDBACK_CLICK);
            return;
        }
        if (a->selecting && g->y >= SELECT_BAR_Y) {
            if (!a->n_selected) {
                return;
            }
            if (ui_hit(s_send, g->x, g->y, 6)) {
                app_feedback(app, FILM_FEEDBACK_CLICK);
                app_share(app, a->selected, a->n_selected, SCR_ALBUM);
            } else if (ui_hit(s_trash, g->x, g->y, 6)) {
                app_feedback(app, FILM_FEEDBACK_CLICK);
                a->confirm_delete = true;
            }
            return;
        }
        const int index = cell_at(app, g->x, g->y);
        if (index < 0) {
            return;
        }
        if (a->selecting) {
            toggle_selected(app, film_library_get(app->library, (size_t)index)->id);
        } else {
            app_open_detail(app, (size_t)index);
        }
        break;
    }
    case GEST_LONG: {
        /* 长按格子直接进入多选并选中它 */
        const int index = cell_at(app, g->x, g->y);
        if (index >= 0 && !a->selecting) {
            a->selecting = true;
            a->n_selected = 0;
            toggle_selected(app, film_library_get(app->library, (size_t)index)->id);
        }
        break;
    }
    case GEST_DRAG_BEGIN:
        a->dragging = true;
        a->drag_start_scroll = a->scroll;
        break;
    case GEST_DRAG: {
        const float max = max_scroll(app);
        float s = a->drag_start_scroll - (float)g->dy;
        a->scroll = s < 0 ? 0 : (s > max ? max : s);
        break;
    }
    case GEST_DRAG_END:
        a->dragging = false;
        a->velocity = -g->vy;
        break;
    default:
        break;
    }
}

static void album_enter(film_app_t *app)
{
    album_state_t *a = &app->album;
    const float max = max_scroll(app);
    if (a->scroll > max) {
        a->scroll = max;
    }
    a->velocity = 0;
    a->dragging = false;
    a->confirm_delete = false;
}

const screen_ops_t g_screen_album = {
    .enter = album_enter,
    .step = album_step,
    .render = album_render,
    .gesture = album_gesture,
    .key = app_key_back_to_camera,
};

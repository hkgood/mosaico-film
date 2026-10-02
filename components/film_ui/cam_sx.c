/*
 * SX-70 宝丽来机身：方形取景、左侧曝光拨轮、右侧计数窗、下方相纸堆 / 红色快门 / 胶片盒
 * （点一下打开全屏胶卷页），以及拍完后的显影过程（相纸从出片口吐出，约 8 秒从灰绿显影成像）。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cam_private.h"

#define SX_VF            gfx_rect(SX_VF_X, 0, FILM_VF_SX_W, FILM_VF_SX_H)
#define SX_WHEEL         gfx_rect(16, 105, 23, 150)
#define EMERGE_MS        900
#define PAPER_ORIGIN_X   240      /*!< 显影相纸的旋转原点（上沿中点） */
#define PAPER_ORIGIN_Y   42
#define PAPER_W          268
#define PAPER_ANGLE      (-1.6f)
#define SLOT_BOTTOM      45
#define WHEEL_RIDGE_PX   6
#define PACK_HIT_R       30
/* 计数窗：机身贴图里原有一个（太靠右上圆角），用皮纹盖掉后在 SX_COUNTER_* 处重画同样的窗 */
#define BAKED_COUNTER    gfx_rect(424, 26, 56, 44)   /*!< 旧窗连同投影 */
#define COUNTER_W        48
#define COUNTER_H        35
#define COUNTER_RADIUS   6
#define COUNTER_BEZEL    0x989C90U
#define COUNTER_EXP_DY   29                          /*!< 窗中心到下方 EXP 刻字 */

/* ---------------------------------------------------------------- 相纸堆 */

/** 把一张缩略图合成成 58×58 的小相纸（纸边 4，下边 12） */
static void compose_print(film_app_t *app, uint16_t *px, uint32_t id)
{
    gfx_canvas_t c;
    gfx_canvas_init(&c, px, SX_PRINT_SIZE, SX_PRINT_SIZE, SX_PRINT_SIZE);
    ui_paper(&c, gfx_rect(0, 0, SX_PRINT_SIZE, SX_PRINT_SIZE), 0, 0);
    const gfx_rect_t img = gfx_rect(4, 4, SX_PRINT_SIZE - 8, SX_PRINT_SIZE - 16);
    const thumb_slot_t *t = id ? app_thumb(app, id) : NULL;
    if (t) {
        const film_photo_t *p = film_library_find(app->library, id, NULL);
        const bool instant = p && (p->flags & FILM_PHOTO_INSTANT);
        /* 宝丽来成片已含相纸边：只取中间的方形图像 */
        const int crop = instant ? t->w * 9 / 10 : (t->w < t->h ? t->w : t->h);
        const gfx_rect_t src = gfx_rect((t->w - crop) / 2, instant ? t->h * 6 / 100 : (t->h - crop) / 2, crop, crop);
        gfx_blit_scaled(&c, t->pixels, t->w, t->h, t->w, src, img, 255);
    } else {
        gfx_fill(&c, img, 0x1A1A19, 255);
    }
    gfx_stroke_round(&c, gfx_rect(img.x - 1, img.y - 1, img.w + 2, img.h + 2), 0, 1, 0x625B50, 72);
    gfx_fill(&c, gfx_rect(img.x, img.y, img.w, 1), COLOR_BLACK, 32);
    gfx_fill(&c, gfx_rect(img.x, img.y, 1, img.h), COLOR_BLACK, 26);
}

void cam_sx_refresh_stack(film_app_t *app)
{
    cam_state_t *cam = &app->cam;
    for (int i = 0; i < 2; ++i) {
        const film_photo_t *p = film_library_get(app->library, (size_t)i);
        const uint32_t id = p ? p->id : 0;
        if (id != cam->stack_ids[i]) {
            cam->stack_ids[i] = id;
            cam->stack_valid[i] = false;
        }
    }
}

static void draw_stack(film_app_t *app, gfx_canvas_t *c)
{
    cam_state_t *cam = &app->cam;
    static const float angles[2] = { -7.0f, 3.0f };
    /* 下面一张是第二新的照片 */
    for (int k = 1; k >= 0; --k) {
        if (!cam->stack_valid[k]) {
            compose_print(app, cam->stack_px[k], cam->stack_ids[k]);
            cam->stack_valid[k] = !cam->stack_ids[k] || app_thumb(app, cam->stack_ids[k]) != NULL;
        }
        gfx_shadow(c, gfx_rect(SX_STACK_X - 29, SX_STACK_Y - 27, 58, 58), 2, 5, COLOR_BLACK, 90);
        gfx_blit_rotated(c, cam->stack_px[k], NULL, SX_PRINT_SIZE, SX_PRINT_SIZE, SX_PRINT_SIZE, SX_STACK_X * 16,
                         SX_STACK_Y * 16, angles[1 - k], 1.0f, 255);
    }
}

/* ---------------------------------------------------------------- 机身部件 */

/** 曝光拨轮：深色圆柱上的横纹随曝光值滚动 */
static void draw_wheel(film_app_t *app, gfx_canvas_t *c)
{
    const gfx_rect_t r = SX_WHEEL;
    gfx_fill_round(c, r, 11, 0x241A13, 255);
    const gfx_rect_t saved = gfx_clip_push(c, gfx_rect(r.x + 2, r.y + 3, r.w - 4, r.h - 6));
    const int offset = (int)lroundf(app->cam.ev * 3.0f * WHEEL_RIDGE_PX);
    for (int y = r.y - WHEEL_RIDGE_PX * 2; y < r.y + r.h + WHEEL_RIDGE_PX; y += WHEEL_RIDGE_PX) {
        const int ry = y + ((offset % WHEEL_RIDGE_PX) + WHEEL_RIDGE_PX) % WHEEL_RIDGE_PX;
        gfx_fill(c, gfx_rect(r.x, ry, r.w, 2), 0x8A7660, 110);
        gfx_fill(c, gfx_rect(r.x, ry + 2, r.w, 1), COLOR_BLACK, 140);
    }
    gfx_clip_pop(c, saved);
    /* 圆柱明暗：两端暗、中间亮 */
    gfx_gradient_v(c, gfx_rect(r.x, r.y, r.w, r.h / 2), 11, COLOR_BLACK, 210, COLOR_BLACK, 0);
    gfx_gradient_v(c, gfx_rect(r.x, r.y + r.h / 2, r.w, r.h / 2), 11, COLOR_BLACK, 0, COLOR_BLACK, 210);
    gfx_gradient_h(c, gfx_rect(r.x, r.y, r.w / 2, r.h), COLOR_WHITE, 28, COLOR_WHITE, 0);
    gfx_stroke_round(c, r, 11, 1, COLOR_BLACK, 120);
}

static void draw_corner_marks(gfx_canvas_t *c)
{
    const gfx_rect_t vf = SX_VF;
    const int s = 14;
    const int in = 12;
    const uint32_t col = 0xFFF8E8;
    const int x0 = vf.x + in;
    const int y0 = vf.y + in;
    const int x1 = vf.x + vf.w - in;
    const int y1 = vf.y + vf.h - in;
    gfx_fill(c, gfx_rect(x0, y0, s, 1), col, 217);
    gfx_fill(c, gfx_rect(x0, y0, 1, s), col, 217);
    gfx_fill(c, gfx_rect(x1 - s, y0, s, 1), col, 217);
    gfx_fill(c, gfx_rect(x1 - 1, y0, 1, s), col, 217);
    gfx_fill(c, gfx_rect(x0, y1 - 1, s, 1), col, 217);
    gfx_fill(c, gfx_rect(x0, y1 - s, 1, s), col, 217);
    gfx_fill(c, gfx_rect(x1 - s, y1 - 1, s, 1), col, 217);
    gfx_fill(c, gfx_rect(x1 - 1, y1 - s, 1, s), col, 217);
}

static void draw_counter_window(gfx_canvas_t *c)
{
    gfx_tile(c, &img_tex_leather, BAKED_COUNTER, 0, 0, 0);
    const gfx_rect_t r = gfx_rect(SX_COUNTER_X - COUNTER_W / 2, SX_COUNTER_Y - COUNTER_H / 2, COUNTER_W, COUNTER_H);
    gfx_shadow(c, gfx_rect(r.x, r.y + 2, r.w, r.h), COUNTER_RADIUS, 4, COLOR_BLACK, 120);
    gfx_fill_round(c, r, COUNTER_RADIUS, COUNTER_BEZEL, 255);
    gfx_gradient_v(c, gfx_rect(r.x + 2, r.y + 2, r.w - 4, r.h - 4), COUNTER_RADIUS - 2, 0x141814, 255, 0x040604,
                   255);
}

/** 胶片盒（sprite + 胶卷色条），cy 为中心 */
static void draw_pack(gfx_canvas_t *c, int cx, int cy, int film)
{
    ui_sprite(c, &img_pack, IMG_PACK_OX, IMG_PACK_OY, cx - 27, cy - 25, 255);
    gfx_fill(c, gfx_rect(cx - 27, cy + 25 - 9 - 4, 54, 4), film_info(film)->label_color, 255);
}

/* ---------------------------------------------------------------- 显影 */

/** 相纸局部坐标（以上沿中点为原点）→ 屏幕坐标（1/16 像素） */
static void paper_point(float lx, float ly, int yoff, int *x, int *y)
{
    const float a = PAPER_ANGLE * 3.14159265f / 180.0f;
    const float cs = cosf(a);
    const float sn = sinf(a);
    *x = (int)lroundf((PAPER_ORIGIN_X + lx * cs - ly * sn) * 16.0f);
    *y = (int)lroundf((PAPER_ORIGIN_Y + yoff + lx * sn + ly * cs) * 16.0f);
}

/** 在相纸上画一个（随相纸旋转的）矩形 */
static void paper_rect(gfx_canvas_t *c, float x, float y, float w, float h, int yoff, uint32_t color, uint8_t alpha)
{
    int p[8];
    paper_point(x, y, yoff, &p[0], &p[1]);
    paper_point(x + w, y, yoff, &p[2], &p[3]);
    paper_point(x + w, y + h, yoff, &p[4], &p[5]);
    paper_point(x, y + h, yoff, &p[6], &p[7]);
    gfx_quad_q4(c, p, color, alpha);
}

/** 显影程度 0..1：直接取暗房全尺寸冲洗的真实进度（相纸上先是小样，雾层随进度褪去） */
static float develop_progress(const film_app_t *app)
{
    return (float)app->cam.dev_progress / (float)FILM_PROGRESS_DONE;
}

static bool develop_done(const film_app_t *app)
{
    return app->cam.dev_progress >= FILM_PROGRESS_DONE;
}

static void draw_developing(film_app_t *app, gfx_canvas_t *c)
{
    cam_state_t *cam = &app->cam;
    gfx_blit(c, &img_dev_bg, 0, 0, 255);
    const float emerge = cam_ease_out(cam_progress(app->now, cam->dev_emerge_t0, EMERGE_MS));
    const int yoff = -(int)lroundf((1.0f - emerge) * 330.0f);
    const gfx_rect_t saved = gfx_clip_push(c, gfx_rect(0, SLOT_BOTTOM - 1, SCREEN_W, SCREEN_H));
    const float half = PAPER_W / 2.0f;
    float img_x = 14.0f - half, img_y = 14.0f, img_w = 240.0f, img_h = 240.0f;
    if (cam->dev_ready && cam->dev_w) {
        /* 成片本身就是一张完整的宝丽来（含相纸边），整张旋转贴上 */
        const float scale = (float)PAPER_W / (float)cam->dev_w;
        const float ph = (float)cam->dev_h * scale;
        int cx, cy;
        paper_point(0.0f, ph / 2.0f, yoff, &cx, &cy);
        gfx_shadow(c, gfx_rect(PAPER_ORIGIN_X - PAPER_W / 2, PAPER_ORIGIN_Y + yoff + 12, PAPER_W, (int)ph), 0, 20,
                   COLOR_BLACK, 110);
        gfx_blit_rotated(c, cam->dev_print, NULL, cam->dev_w, cam->dev_h, cam->dev_w, cx, cy, PAPER_ANGLE, scale, 255);
        /* 相纸成片的图像区（与 film_print 的版式一致：左右 5.1%，上 5.6%，宽 89.8%） */
        img_w = (float)PAPER_W * 0.898f;
        img_h = img_w;
        img_x = (float)PAPER_W * 0.051f - half;
        img_y = ph * 0.056f;
    } else {
        ui_sprite(c, &img_dev_paper, 0, 0, IMG_DEV_PAPER_OX, IMG_DEV_PAPER_OY + yoff, 255);
    }
    const float t = develop_progress(app);
    const float e = cam_ease_in_out(t);
    const uint8_t veil = (uint8_t)lroundf((1.0f - e) * 255.0f);
    if (veil) {
        paper_rect(c, img_x, img_y, img_w, img_h, yoff, COLOR_UNDEVELOPED, veil);
    }
    gfx_clip_pop(c, saved);

    /* 进度线与说明 */
    if (develop_done(app)) {
        /* 完成态按最终样稿落在暖棕底板上，与冲洗中的悬浮进度信息明确区分。 */
        gfx_fill(c, gfx_rect(126, 404, 228, 70), 0x5D3B25, 255);
        gfx_fill_round(c, gfx_rect(160, 419, 160, 3), 1, COLOR_AMBER, 255);
        const gfx_text_style_t st =
            ui_style(&font_jost_r12, 12, 0.0f, COLOR_CREAM, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &st, 240, 452, "DEVELOPED");
    } else {
        gfx_fill_round(c, gfx_rect(160, 402, 160, 2), 1, COLOR_BLACK, 89);
        const int w = (int)lroundf(160.0f * t);
        if (w > 0) {
            gfx_glow(c, 160 + w, 403, 8, COLOR_AMBER, 90);
            gfx_fill_round(c, gfx_rect(160, 402, w, 2), 1, COLOR_AMBER, 255);
        }
        char label[24];
        snprintf(label, sizeof(label), "DEVELOPING  %u%%", (unsigned)(cam->dev_progress / 10));
        const gfx_text_style_t st = ui_style(&font_jost_m10, 10, 0.42f, 0xF1E4CD, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
        gfx_text(c, &st, 244, 425, label);
    }
}

bool cam_sx_step(film_app_t *app)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay != CAM_OVL_DEVELOPING) {
        return false;
    }
    if (develop_done(app)) {
        if (!cam->dev_done_at) {
            cam->dev_done_at = app->now;
        } else if (app->now - cam->dev_done_at >= DEVELOP_LINGER_MS) {
            cam_close(app);
            cam_sx_refresh_stack(app);
            if (cam->shot_pending_rollend) {
                cam->shot_pending_rollend = false;
                cam_open(app, CAM_OVL_ROLLEND);
            }
            return true;
        }
    }
    /* 只有相纸吐出的动画需要逐帧画；显影雾层由进度事件驱动重绘，把 CPU 留给冲洗 */
    return (int32_t)(app->now - cam->dev_emerge_t0) < EMERGE_MS + 40;   /* 快门帘期间 t0 还在未来 */
}

void cam_sx_event(film_app_t *app, const film_event_t *ev)
{
    cam_state_t *cam = &app->cam;
    if (ev->type == FILM_EVT_SAVED) {
        cam_sx_refresh_stack(app);
        return;
    }
    if (cam->overlay != CAM_OVL_DEVELOPING || ev->photo_id != cam->dev_id) {
        return;
    }
    if (ev->type == FILM_EVT_DEVELOPED && !ev->preview && ev->screen &&
        (size_t)ev->width * ev->height <= (size_t)FILM_VF_M6_W * FILM_VF_M6_H) {
        memcpy(cam->dev_print, ev->screen, sizeof(uint16_t) * ev->width * ev->height);
        cam->dev_w = ev->width;
        cam->dev_h = ev->height;
        cam->dev_ready = true;
    } else if (ev->type == FILM_EVT_PROGRESS) {
        if (ev->progress > cam->dev_progress) {
            cam->dev_progress = ev->progress > FILM_PROGRESS_DONE ? FILM_PROGRESS_DONE : ev->progress;
        }
    } else if (ev->type == FILM_EVT_SHOT_FAILED) {
        cam_close(app);
        cam->shot_pending_rollend = false;
    }
}

/* ---------------------------------------------------------------- 绘制与手势 */

void cam_sx_render(film_app_t *app, gfx_canvas_t *c)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay == CAM_OVL_DEVELOPING) {
        draw_developing(app, c);
        return;
    }
    const gfx_rot_t rot = app->rot;
    gfx_blit(c, &img_sx_body, 0, 0, 255);
    if (cam->shot == SHOT_FLASH) {
        gfx_fill(c, SX_VF, COLOR_BLACK, 255);
    } else {
        cam_draw_live(app, c, SX_VF);
        draw_corner_marks(c);
        cam_draw_vf_overlay(app, c, SX_VF, false);
    }
    draw_wheel(app, c);

    draw_counter_window(c);
    char n[8];
    snprintf(n, sizeof(n), "%u", (unsigned)app->settings.frame);
    const gfx_text_style_t cnt = ui_style(&font_jost_m18, 18, 0.0f, 0xEFE6CF, 255, GFX_ALIGN_CENTER, rot);
    gfx_text(c, &cnt, SX_COUNTER_X, SX_COUNTER_Y, n);
    ui_engrave(c, SX_COUNTER_X, SX_COUNTER_Y + COUNTER_EXP_DY, "EXP", true, GFX_ROT_0);
    const gfx_text_style_t deco =
        ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.36f, COLOR_ENGRAVE_LT, 140, GFX_ALIGN_CENTER, GFX_ROT_90);
    gfx_text(c, &deco, SX_COUNTER_X, 190, "INSTANT");

    draw_stack(app, c);
    if (cam->shutter_down) {
        ui_sprite(c, &img_sx_shutter_down, IMG_SX_SHUTTER_DOWN_OX, IMG_SX_SHUTTER_DOWN_OY, SX_SHUTTER_X - 40,
                  SX_SHUTTER_Y - 40, 255);
    } else {
        ui_sprite(c, &img_sx_shutter, IMG_SX_SHUTTER_OX, IMG_SX_SHUTTER_OY, SX_SHUTTER_X - 40, SX_SHUTTER_Y - 40, 255);
    }
    if (cam->pack_down || cam->overlay == CAM_OVL_POPOVER) {
        gfx_glow(c, SX_PACK_X, SX_PACK_Y, 40, COLOR_AMBER, 70);
    }
    draw_pack(c, SX_PACK_X, SX_PACK_Y, app->settings.film);
    int dx, dy;
    ui_rot_vec(rot, 0, 33, &dx, &dy);
    ui_engrave(c, SX_PACK_X + dx, SX_PACK_Y + dy, film_info(app->settings.film)->name, true, rot);

    if (cam->overlay == CAM_OVL_POPOVER) {
        gfx_dim(c, SX_VF, 184);
        cam_draw_popover(app, c, SX_PACK_X - POPOVER_CARET_DX);
    } else if (cam->overlay == CAM_OVL_ROLLEND) {
        cam_draw_rollend(app, c);
    }
}

static bool hit_circle(int x, int y, int cx, int cy, int r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

void cam_sx_gesture(film_app_t *app, const gesture_t *g)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay == CAM_OVL_DEVELOPING) {
        /* 冲洗中不能跳过（全尺寸还在算）；冲完后轻点立即回取景 */
        if (g->kind == GEST_TAP && develop_done(app)) {
            cam->dev_done_at = app->now - DEVELOP_LINGER_MS;
        }
        return;
    }
    if (cam_overlay_gesture(app, g, SX_PACK_X - POPOVER_CARET_DX)) {
        return;
    }
    const bool in_vf_or_wheel = g->y0 < FILM_VF_SX_H;
    switch (g->kind) {
    case GEST_PRESS:
        cam->shutter_down = hit_circle(g->x, g->y, SX_SHUTTER_X, SX_SHUTTER_Y, 44);
        cam->pack_down = hit_circle(g->x, g->y, SX_PACK_X, SX_PACK_Y, PACK_HIT_R);
        cam->counter_down = hit_circle(g->x, g->y, SX_STACK_X, SX_STACK_Y, PACK_HIT_R + 4);
        break;
    case GEST_LONG:
        if (cam->pack_down) {
            cam_open(app, CAM_OVL_POPOVER);
            app_feedback(app, FILM_FEEDBACK_CLICK);
        }
        break;
    case GEST_TAP:
        if (cam->shutter_down) {
            cam_fire(app);
        } else if (cam->pack_down) {
            app_feedback(app, FILM_FEEDBACK_CLICK);
            app_open_film(app, SCR_CAMERA, app->settings.film);
        } else if (cam->counter_down) {
            app->album.selecting = false;
            app->album.scroll = 0;
            app_go(app, SCR_ALBUM);
        }
        break;
    case GEST_DOUBLE_TAP:
        if (in_vf_or_wheel) {
            cam_set_ev(app, 0.0f);
        }
        break;
    case GEST_DRAG_BEGIN:
        cam->shutter_down = cam->counter_down = false;
        if (g->horizontal && (in_vf_or_wheel || cam->pack_down)) {
            cam->dragging_film = true;
            cam->film_drag_start = app->settings.film;
        } else if (!g->horizontal && in_vf_or_wheel) {
            cam->dragging_ev = true;
            cam->ev_drag_start = cam->ev;
        }
        cam->pack_down = false;
        break;
    case GEST_DRAG:
        if (cam->dragging_film) {
            app_select_film(app, cam->film_drag_start - g->du / FILM_SWIPE_PX);
        } else if (cam->dragging_ev) {
            cam_set_ev(app, cam->ev_drag_start - (float)g->dv / EV_PX_PER_STOP);
        }
        break;
    case GEST_DRAG_END:
        cam->dragging_film = cam->dragging_ev = false;
        break;
    case GEST_RELEASE:
        cam->shutter_down = cam->pack_down = cam->counter_down = false;
        break;
    }
}

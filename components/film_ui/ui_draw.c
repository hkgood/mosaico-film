/*
 * 各页面共用的绘制部件：坐标转正、刻字、按钮、拨杆、页眉、提示条，以及胶卷文字表。
 * 尺寸与颜色取自已确认的 v3 样稿（.agents/analysis/lomo_camera/mockups/screens/v3.css）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_private.h"

/* ---------------------------------------------------------------- 胶卷文字 */

static const film_info_t s_films[FILM_ID_COUNT] = {
    [FILM_ID_GOLD] = { "GOLD 200", "GOLD", "200", "ISO 200", "Warm highlights, creamy skin. Made for sunshine.",
                       0xE8B21C, &img_can_gold, &img_can_gold_s },
    [FILM_ID_PORTRA] = { "SOFT 400", "SOFT", "400", "ISO 400", "Low contrast and gentle pastel tones.", 0xD9774A,
                         &img_can_soft, &img_can_soft_s },
    [FILM_ID_GREEN] = { "VERDE 200", "VERDE", "200", "ISO 200", "Cool cyan shadows and fresh greens.", 0x2F9A6A,
                        &img_can_verde, &img_can_verde_s },
    [FILM_ID_CROSS] = { "CROSS X", "CROSS", "X", "ISO 100", "Cross-processed: yellow highs, blue lows.", 0xC2185B,
                        &img_can_cross, &img_can_cross_s },
    [FILM_ID_BW] = { "SILVER 400", "SILVER", "400", "ISO 400", "Deep blacks with bold grain.", 0x3A3A3A,
                     &img_can_silver, &img_can_silver_s },
    [FILM_ID_FADED] = { "FADED 77", "FADED", "77", "ISO 100", "Lifted blacks and a magenta-orange fade.", 0xB5651D,
                        &img_can_faded, &img_can_faded_s },
    [FILM_ID_NIGHT] = { "NIGHT 800T", "NIGHT", "800T", "ISO 800", "Tungsten balance with red halation.", 0x1E5AA8,
                        &img_can_night, &img_can_night_s },
    [FILM_ID_PIXEL] = { "PIXEL 8BIT", "PIXEL", "8BIT", "8 BIT", "32-colour palette with light dithering.", 0x7B3FE4,
                        &img_can_pixel, &img_can_pixel_s },
};

int film_wrap(int film)
{
    return ((film % FILM_ID_COUNT) + FILM_ID_COUNT) % FILM_ID_COUNT;
}

const film_info_t *film_info(int film)
{
    return &s_films[film_wrap(film)];
}

/* ---------------------------------------------------------------- 坐标转正 */

ui_frame_t ui_frame(gfx_rect_t screen, gfx_rot_t rot)
{
    const bool swap = rot == GFX_ROT_90 || rot == GFX_ROT_270;
    return (ui_frame_t) { screen, rot, swap ? screen.h : screen.w, swap ? screen.w : screen.h };
}

void ui_rot_vec(gfx_rot_t rot, int du, int dv, int *dx, int *dy)
{
    switch (rot) {
    case GFX_ROT_90:    /* 用户的 x 轴朝屏幕下方，y 轴朝屏幕左方 */
        *dx = -dv;
        *dy = du;
        break;
    case GFX_ROT_180:
        *dx = -du;
        *dy = -dv;
        break;
    case GFX_ROT_270:
        *dx = dv;
        *dy = -du;
        break;
    default:
        *dx = du;
        *dy = dv;
        break;
    }
}

void ui_unrot_vec(gfx_rot_t rot, int dx, int dy, int *du, int *dv)
{
    switch (rot) {
    case GFX_ROT_90:
        *du = dy;
        *dv = -dx;
        break;
    case GFX_ROT_180:
        *du = -dx;
        *dv = -dy;
        break;
    case GFX_ROT_270:
        *du = -dy;
        *dv = dx;
        break;
    default:
        *du = dx;
        *dv = dy;
        break;
    }
}

void ui_point(const ui_frame_t *f, int u, int v, int *x, int *y)
{
    const gfx_rect_t r = f->screen;
    switch (f->rot) {
    case GFX_ROT_90:
        *x = r.x + r.w - v;
        *y = r.y + u;
        break;
    case GFX_ROT_180:
        *x = r.x + r.w - u;
        *y = r.y + r.h - v;
        break;
    case GFX_ROT_270:
        *x = r.x + v;
        *y = r.y + r.h - u;
        break;
    default:
        *x = r.x + u;
        *y = r.y + v;
        break;
    }
}

gfx_rect_t ui_rect(const ui_frame_t *f, int u, int v, int w, int h)
{
    int x0, y0, x1, y1;
    ui_point(f, u, v, &x0, &y0);
    ui_point(f, u + w, v + h, &x1, &y1);
    const int lx = x0 < x1 ? x0 : x1;
    const int ly = y0 < y1 ? y0 : y1;
    return gfx_rect(lx, ly, x0 < x1 ? x1 - x0 : x0 - x1, y0 < y1 ? y1 - y0 : y0 - y1);
}

bool ui_hit(gfx_rect_t r, int x, int y, int slop)
{
    return x >= r.x - slop && x < r.x + r.w + slop && y >= r.y - slop && y < r.y + r.h + slop;
}

bool ui_back_hit(int x, int y)
{
    return x >= 0 && x < BACK_HIT_W && y >= 0 && y < BACK_HIT_H;
}

/* ---------------------------------------------------------------- 圆角安全区 */

/** 点到"安全区核心矩形"（安全区再内缩 SAFE_RADIUS）的距离平方 */
static int safe_core_dist2(int x, int y)
{
    const int lo = SAFE_INSET + SAFE_RADIUS;
    const int hi_x = SCREEN_W - SAFE_INSET - SAFE_RADIUS;
    const int hi_y = SCREEN_H - SAFE_INSET - SAFE_RADIUS;
    const int dx = x < lo ? lo - x : (x > hi_x ? x - hi_x : 0);
    const int dy = y < lo ? lo - y : (y > hi_y ? y - hi_y : 0);
    return dx * dx + dy * dy;
}

bool ui_in_safe_area(gfx_rect_t r, int radius)
{
    /* 圆角矩形在安全区内 ⇔ 它四个角的圆心离核心矩形不超过 SAFE_RADIUS - radius */
    if (radius > SAFE_RADIUS) {
        radius = SAFE_RADIUS;
    }
    const int limit = SAFE_RADIUS - radius;
    const int xs[2] = { r.x + radius, r.x + r.w - radius };
    const int ys[2] = { r.y + radius, r.y + r.h - radius };
    for (int i = 0; i < 4; ++i) {
        if (safe_core_dist2(xs[i & 1], ys[i >> 1]) > limit * limit) {
            return false;
        }
    }
    return true;
}

/**
 * 仅模拟器：控件画到圆角安全区外时打一行警告（film_ui_script 会检查日志里没有这一行）。
 * 设备上不检查，避免每帧多余开销。
 */
void ui_safe_check(gfx_rect_t r, int radius, const char *what)
{
#ifndef ESP_PLATFORM
    if (!ui_in_safe_area(r, radius)) {
        fprintf(stderr, "ui-safe-area: %s at (%d,%d %dx%d) is outside the rounded-corner safe area\n",
                what ? what : "control", r.x, r.y, r.w, r.h);
    }
#else
    (void)r;
    (void)radius;
    (void)what;
#endif
}

/* ---------------------------------------------------------------- 文字 */

gfx_text_style_t ui_style(const gfx_font_t *font, float size_px, float tracking_em, uint32_t color, uint8_t alpha,
                          gfx_align_t align, gfx_rot_t rot)
{
    return (gfx_text_style_t) {
        .font = font,
        .fallback = NULL,
        .color = color,
        .alpha = alpha,
        .tracking_q4 = (int16_t)lroundf(size_px * tracking_em * 16.0f),
        .align = align,
        .rot = rot,
    };
}

gfx_text_style_t ui_cjk(const gfx_font_t *font, uint32_t color, uint8_t alpha, gfx_align_t align, gfx_rot_t rot)
{
    gfx_text_style_t s = ui_style(font, 12, 0.0f, color, alpha, align, rot);
    s.fallback = font == &font_noto_11 ? &font_jost_r11 : &font_jost_r12;
    return s;
}

/** 在"用户方向"偏移 (du, dv) 处画一遍文字（刻字高光/阴影用） */
static void text_offset(gfx_canvas_t *c, gfx_text_style_t style, int x, int y, int du, int dv, uint32_t color,
                        uint8_t alpha, const char *text)
{
    int dx, dy;
    ui_rot_vec(style.rot, du, dv, &dx, &dy);
    style.color = color;
    style.alpha = alpha;
    gfx_text(c, &style, x + dx, y + dy, text);
}

void ui_engrave(gfx_canvas_t *c, int x, int y, const char *text, bool light, gfx_rot_t rot)
{
    const gfx_text_style_t st = ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.22f,
                                         light ? COLOR_ENGRAVE_LT : COLOR_ENGRAVE, 255, GFX_ALIGN_CENTER, rot);
    if (light) {
        text_offset(c, st, x, y, 0, -1, COLOR_BLACK, 178, text);
    } else {
        text_offset(c, st, x, y, 0, 1, COLOR_WHITE, 178, text);
    }
    gfx_text(c, &st, x, y, text);
}

void ui_glow_text(gfx_canvas_t *c, const gfx_font_t *core, const gfx_font_t *glow, float size, float tracking,
                  uint32_t color, uint8_t glow_alpha, gfx_align_t align, gfx_rot_t rot, int x, int y,
                  const char *text)
{
    if (glow && glow_alpha) {
        const gfx_text_style_t gs = ui_style(glow, size, tracking, color, glow_alpha, align, rot);
        gfx_text(c, &gs, x, y, text);
    }
    const gfx_text_style_t st = ui_style(core, size, tracking, color, 255, align, rot);
    gfx_text(c, &st, x, y, text);
}

/* ---------------------------------------------------------------- 按钮 */

#define BUTTON_RADIUS 10

/** 图标 + 文字水平居中排在按钮里 */
static void button_label(gfx_canvas_t *c, gfx_rect_t r, const char *label, const gfx_image_t *icon, uint32_t ink,
                         uint8_t alpha, bool engraved)
{
    const gfx_text_style_t st = ui_style(&font_jost_r16, UI_TEXT_BUTTON, 0.02f, ink, alpha, GFX_ALIGN_LEFT, GFX_ROT_0);
    const int text_w = label && *label ? gfx_text_width_q4(&st, label) / 16 : 0;
    const int icon_w = icon ? icon->width : 0;
    const int gap = icon && text_w ? 8 : 0;
    int x = r.x + (r.w - (icon_w + gap + text_w)) / 2;
    const int cy = r.y + r.h / 2;
    if (icon) {
        gfx_icon(c, icon, x + icon_w / 2, cy, GFX_ROT_0, ink, alpha);
        x += icon_w + gap;
    }
    if (text_w) {
        if (engraved) {
            text_offset(c, st, x, cy, 0, 1, COLOR_WHITE, (uint8_t)(alpha * 7 / 10), label);
        }
        gfx_text(c, &st, x, cy, label);
    }
}

void ui_button_primary(gfx_canvas_t *c, gfx_rect_t r, const char *label, const gfx_image_t *icon, bool pressed,
                       bool enabled)
{
    ui_safe_check(r, BUTTON_RADIUS, label);
    gfx_shadow(c, gfx_rect(r.x, r.y + 3, r.w, r.h), BUTTON_RADIUS, 8, COLOR_BLACK, enabled ? 115 : 50);
    gfx_stroke_round(c, gfx_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), BUTTON_RADIUS + 1, 1, COLOR_BLACK, 128);
    gfx_tile(c, &img_tex_alu, r, r.x + r.w / 2 - 240, r.y + r.h / 2 - 240, BUTTON_RADIUS);
    gfx_gradient_v(c, r, BUTTON_RADIUS, COLOR_WHITE, 40, COLOR_BLACK, 20);
    gfx_fill(c, gfx_rect(r.x + BUTTON_RADIUS, r.y, r.w - 2 * BUTTON_RADIUS, 1), COLOR_WHITE, 204);
    if (pressed) {
        gfx_fill_round(c, r, BUTTON_RADIUS, COLOR_BLACK, 40);
    }
    button_label(c, r, label, icon, COLOR_ENGRAVE, enabled ? 255 : 110, true);
    if (!enabled) {
        gfx_fill_round(c, r, BUTTON_RADIUS, COLOR_INK, 90);
    }
}

void ui_button_line(gfx_canvas_t *c, gfx_rect_t r, const char *label, const gfx_image_t *icon, bool pressed,
                    uint32_t ink, uint8_t line_alpha)
{
    ui_safe_check(r, BUTTON_RADIUS, label);
    if (pressed) {
        gfx_fill_round(c, r, BUTTON_RADIUS, ink, 30);
    }
    gfx_stroke_round(c, r, BUTTON_RADIUS, 1, ink, line_alpha);
    button_label(c, r, label, icon, ink, 255, false);
}

void ui_button_dark(gfx_canvas_t *c, gfx_rect_t r, const char *label, bool pressed, bool enabled)
{
    ui_safe_check(r, BUTTON_RADIUS, label);
    gfx_shadow(c, gfx_rect(r.x, r.y + 2, r.w, r.h), BUTTON_RADIUS, 5, COLOR_BLACK, 90);
    gfx_stroke_round(c, gfx_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), BUTTON_RADIUS + 1, 1, 0x050505, 255);
    gfx_gradient_v(c, r, BUTTON_RADIUS, pressed ? 0x1C1C1A : 0x2C2C2A, 255, 0x0C0C0B, 255);
    gfx_fill(c, gfx_rect(r.x + BUTTON_RADIUS, r.y, r.w - 2 * BUTTON_RADIUS, 1), COLOR_WHITE, 36);
    const gfx_text_style_t st = ui_style(&font_jost_r16, UI_TEXT_BUTTON, 0.02f, COLOR_CREAM, enabled ? 255 : 100,
                                         GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &st, r.x + r.w / 2, r.y + r.h / 2, label);
}

void ui_icon_button(gfx_canvas_t *c, int x, int y, const gfx_image_t *icon, bool pressed)
{
    ui_safe_check(gfx_rect(x, y, 36, 36), 18, "icon button");
    const int cx = (x + 18) * 16;
    const int cy = (y + 18) * 16;
    gfx_circle_q4(c, cx, cy, 18 * 16, 0x0A0A09, pressed ? 200 : 140);
    gfx_ring_q4(c, cx, cy, 18 * 16, 16, COLOR_WHITE, 46);
    gfx_icon(c, icon, x + 18, y + 18, GFX_ROT_0, COLOR_CREAM, 255);
}

void ui_check(gfx_canvas_t *c, int x, int y, bool on)
{
    const int cx = (x + 11) * 16;
    const int cy = (y + 11) * 16;
    gfx_circle_q4(c, cx, cy + 16, 12 * 16, COLOR_BLACK, 70);
    if (on) {
        gfx_circle_q4(c, cx, cy, 11 * 16, COLOR_AMBER, 255);
        gfx_icon(c, &img_icon_check, x + 11, y + 11, GFX_ROT_0, 0x1A0E03, 255);
    } else {
        gfx_circle_q4(c, cx, cy, 11 * 16, COLOR_BLACK, 64);
        gfx_ring_q4(c, cx, cy, 11 * 16, 24, COLOR_WHITE, 230);
    }
}

void ui_sprite(gfx_canvas_t *c, const gfx_image_t *img, int ox, int oy, int x, int y, uint8_t opacity)
{
    gfx_blit(c, img, x + ox, y + oy, opacity);
}

void ui_lever(gfx_canvas_t *c, int cx, int cy, bool on)
{
    if (on) {
        ui_sprite(c, &img_lever_on, IMG_LEVER_ON_OX, IMG_LEVER_ON_OY, cx - 18, cy - 9, 255);
    } else {
        ui_sprite(c, &img_lever_off, IMG_LEVER_OFF_OX, IMG_LEVER_OFF_OY, cx - 18, cy - 9, 255);
    }
}

void ui_index_mark(gfx_canvas_t *c, int cx, int top, gfx_rot_t rot)
{
    /* 样稿：宽 8、高 5 的琥珀三角，下方 1px 白色高光；以 (cx, top) 为"上沿中点" */
    const int pts[3][2] = { { -4 * 16, 0 }, { 4 * 16, 0 }, { 0, 5 * 16 } };
    int sx[3], sy[3];
    for (int i = 0; i < 3; ++i) {
        ui_rot_vec(rot, pts[i][0], pts[i][1], &sx[i], &sy[i]);
    }
    int hx, hy;
    ui_rot_vec(rot, 0, 16, &hx, &hy);
    const int bx = cx * 16;
    const int by = top * 16;
    gfx_triangle_q4(c, bx + sx[0] + hx, by + sy[0] + hy, bx + sx[1] + hx, by + sy[1] + hy, bx + sx[2] + hx,
                    by + sy[2] + hy, COLOR_WHITE, 128);
    gfx_triangle_q4(c, bx + sx[0], by + sy[0], bx + sx[1], by + sy[1], bx + sx[2], by + sy[2], COLOR_AMBER, 255);
}

/* ---------------------------------------------------------------- 页面部件 */

void ui_toast(gfx_canvas_t *c, const ui_frame_t *f, const char *text, uint8_t alpha)
{
    if (!text[0] || alpha == 0) {
        return;
    }
    const gfx_text_style_t st = ui_cjk(&font_noto_12, COLOR_CREAM, alpha, GFX_ALIGN_CENTER, f->rot);
    const int w = gfx_text_width_q4(&st, text) / 16 + 32;
    const gfx_rect_t r = ui_rect(f, (f->w - w) / 2, 30, w, 30);
    gfx_fill_round(c, r, 15, COLOR_INK, (uint8_t)(alpha * 200 / 255));
    gfx_stroke_round(c, r, 15, 1, COLOR_WHITE, (uint8_t)(alpha * 30 / 255));
    gfx_text(c, &st, r.x + r.w / 2, r.y + r.h / 2, text);
}

void ui_top_shade(gfx_canvas_t *c, int height, uint8_t alpha)
{
    gfx_gradient_v(c, gfx_rect(0, 0, SCREEN_W, height), 0, COLOR_BLACK, alpha, COLOR_BLACK, 0);
}

void ui_header(gfx_canvas_t *c, const char *title, const char *sub, const char *right, bool back_pressed)
{
    gfx_tile(c, &img_tex_vulc, gfx_rect(0, 0, SCREEN_W, HEADER_H), 0, 0, 0);
    gfx_fill(c, gfx_rect(0, HEADER_H, SCREEN_W, 1), COLOR_WHITE, 20);
    ui_icon_button(c, HEADER_BACK_X, HEADER_BACK_Y, &img_icon_back, back_pressed);
    const gfx_text_style_t t = ui_style(&font_jost_m12, 12, 0.22f, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &t, HEADER_TEXT_X, HEADER_TITLE_Y, title);
    const gfx_text_style_t s =
        ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.22f, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &s, HEADER_TEXT_X, HEADER_SUB_Y, sub);
    if (right && *right) {
        const gfx_text_style_t r =
            ui_style(&font_jost_m14, UI_TEXT_LINK, 0.12f, COLOR_CREAM, 255, GFX_ALIGN_RIGHT, GFX_ROT_0);
        gfx_text(c, &r, HEADER_RIGHT_X, HEADER_RIGHT_Y, right);
    }
}

void ui_paper(gfx_canvas_t *c, gfx_rect_t r, int shadow_blur, uint8_t shadow_alpha)
{
    if (shadow_blur) {
        gfx_shadow(c, gfx_rect(r.x, r.y + shadow_blur / 3, r.w, r.h), 0, shadow_blur, COLOR_BLACK, shadow_alpha);
    }
    gfx_tile(c, &img_tex_paper, r, r.x, r.y, 0);
    /* 新鲜相纸的压膜边和纸张厚度：上/左偏暗，下/右有很薄的纤维高光。 */
    gfx_fill(c, gfx_rect(r.x, r.y, r.w, 1), 0x6A6256, 24);
    gfx_fill(c, gfx_rect(r.x, r.y, 1, r.h), 0x6A6256, 20);
    gfx_fill(c, gfx_rect(r.x, r.y + r.h - 1, r.w, 1), COLOR_WHITE, 105);
    gfx_fill(c, gfx_rect(r.x + r.w - 1, r.y, 1, r.h), COLOR_WHITE, 78);
}

/* 删除确认弹窗：居中卡片，按钮落在卡片下半部 */
static const gfx_rect_t s_confirm_card = { 90, 150, 300, 160 };
static const gfx_rect_t s_confirm_cancel = { 110, 246, 120, 40 };
static const gfx_rect_t s_confirm_delete = { 250, 246, 120, 40 };

void ui_delete_confirm(gfx_canvas_t *c, const char *question)
{
    const gfx_rect_t card = s_confirm_card;
    gfx_fill(c, gfx_rect(0, 0, SCREEN_W, SCREEN_H), COLOR_BLACK, 140);
    gfx_shadow(c, gfx_rect(card.x, card.y + 8, card.w, card.h), 14, 24, COLOR_BLACK, 160);
    gfx_tile(c, &img_tex_vulc, card, 0, 0, 14);
    gfx_stroke_round(c, card, 14, 1, COLOR_WHITE, 30);
    const gfx_text_style_t q = ui_cjk(&font_noto_13, COLOR_CREAM, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &q, SCREEN_W / 2, card.y + 42, question);
    const gfx_text_style_t s = ui_cjk(&font_noto_12, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &s, SCREEN_W / 2, card.y + 66, "原片与成片都会删除，无法恢复");
    ui_button_line(c, s_confirm_cancel, "CANCEL", NULL, false, COLOR_CREAM, 71);
    ui_button_line(c, s_confirm_delete, "DELETE", NULL, false, COLOR_LED, 160);
}

bool ui_delete_confirm_hit(int x, int y)
{
    return ui_hit(s_confirm_delete, x, y, 6);
}

/* ---------------------------------------------------------------- 格式化 */

void ui_format_ev(float ev, char *buf, size_t len)
{
    const int tenths = (int)lroundf(ev * 10.0f);
    if (tenths == 0) {
        snprintf(buf, len, "0");
    } else if (tenths % 10 == 0) {
        snprintf(buf, len, "%+d", tenths / 10);
    } else {
        snprintf(buf, len, "%c%d.%d", tenths > 0 ? '+' : '-', abs(tenths) / 10, abs(tenths) % 10);
    }
}

void ui_format_stamp(int64_t time_s, char *buf, size_t len)
{
    const time_t t = (time_t)time_s;
    struct tm tm;
    if (time_s <= 0 || !localtime_r(&t, &tm)) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, len, "'%02d %d %d", tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday);
}

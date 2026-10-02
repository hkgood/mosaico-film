/*
 * 机身选择面板：从屏幕顶边向下拉出，里面陈列两台相机（M6 旁轴 / SX-70 拍立得），点选即换机身。
 *
 * 交互：
 *   - 取景页没有其他弹出层、不在拍摄时，手指从顶边 PICKER_EDGE_PX 以内按下并向下拖 → 面板跟手拉出；
 *     松手时拉过 PICKER_OPEN_RATIO 或向下甩动就展开，否则收回。
 *   - 展开后：点卡片换机身（皮革合拢动画由 cam_switch_body 负责）；点面板下方空白、或向上推 → 收起。
 * 面板按屏幕物理方向绘制（与弹出面板、一卷拍完一致），不随握持方向旋转。
 *
 * 性能：面板不透明，取景页只画面板下方露出的部分（cam_picker_covered_rows 给出裁剪线）；
 * 打开期间取景定格（app.c），面板静止时整页不再重绘。
 */
#include <math.h>
#include <stdlib.h>

#include "cam_private.h"

#define PICKER_EDGE_PX      36      /*!< 顶边多高范围内按下才算"下拉" */
#define PICKER_SHEET_H      300     /*!< 面板高度 */
#define PICKER_RADIUS       22
#define PICKER_OPEN_RATIO   0.38f   /*!< 松手时拉出超过这个比例就展开 */
#define PICKER_FLING_PX_S   600.0f  /*!< 甩动速度阈值（像素/秒） */
#define PICKER_ANIM_MS      90.0f   /*!< 展开/收起的跟随时间常数 */
#define PICKER_DIM_MAX      120     /*!< 完全展开时背后机身压暗的程度（/256） */
#define PICKER_SHADOW_H     18      /*!< 面板下沿投影的高度（线性渐变，代替逐像素算距离的柔和投影） */
#define PICKER_SHADOW_ALPHA 120

#define CARD_W              214
#define CARD_H              190
#define CARD_Y              49      /*!< 相对面板顶部 */
#define CARD_GAP            8
#define CARD_X0             ((SCREEN_W - 2 * CARD_W - CARD_GAP) / 2)
#define CARD_RADIUS         7
#define ART_W               166     /*!< 卡片里相机插画的最大宽度 */
#define ART_H               112

#define PICKER_VISIBLE_PULL 0.002f  /*!< 展开程度不超过它就当作完全收起，不画面板 */

#define HINT_W              34      /*!< 顶边下拉提示条 */
#define HINT_H              4
#define HINT_Y              6

typedef struct {
    const char *name;
    const char *sub;
    bool instant;
} body_info_t;

static const body_info_t k_bodies[2] = {
    { "M6", "RANGEFINDER  \xC2\xB7  4 : 3", false },
    { "SX-70", "INSTANT  \xC2\xB7  1 : 1", true },
};

/* ---------------------------------------------------------------- 几何 */

/** 面板顶边的屏幕 y（展开时为 0，收起时为 -PICKER_SHEET_H） */
static int sheet_top(const cam_state_t *cam)
{
    return (int)lroundf((cam->picker_pull - 1.0f) * PICKER_SHEET_H);
}

int cam_picker_covered_rows(const cam_state_t *cam)
{
    if (cam->picker_pull <= PICKER_VISIBLE_PULL) {
        return 0;
    }
    /* 下沿两个圆角处会露出背后，从圆角开始的那几行仍要画取景页 */
    const int rows = sheet_top(cam) + PICKER_SHEET_H - PICKER_RADIUS;
    return rows < 0 ? 0 : rows;
}

static gfx_rect_t card_rect(const cam_state_t *cam, int i)
{
    return gfx_rect(CARD_X0 + i * (CARD_W + CARD_GAP), sheet_top(cam) + CARD_Y, CARD_W, CARD_H);
}

/** 点中的卡片编号，没点中返回 -1 */
static int card_at(const cam_state_t *cam, int x, int y)
{
    for (int i = 0; i < 2; ++i) {
        if (ui_hit(card_rect(cam, i), x, y, 4)) {
            return i;
        }
    }
    return -1;
}

static void circle(gfx_canvas_t *c, int cx, int cy, int r, uint32_t color, uint8_t alpha)
{
    gfx_circle_q4(c, cx * 16, cy * 16, r * 16, color, alpha);
}

static void quad(gfx_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3,
                 uint32_t color, uint8_t alpha)
{
    const int pts[8] = { x0 * 16, y0 * 16, x1 * 16, y1 * 16, x2 * 16, y2 * 16, x3 * 16, y3 * 16 };
    gfx_quad_q4(c, pts, color, alpha);
}

/* ---------------------------------------------------------------- 相机插画 */

/** 镜头：外圈镜筒、金属环、黑色内圈、深色玻璃和一点高光 */
static void draw_lens(gfx_canvas_t *c, int cx, int cy, int r, uint32_t ring)
{
    circle(c, cx, cy + 2, r + 1, COLOR_BLACK, 90);
    circle(c, cx, cy, r, 0x141413, 255);
    circle(c, cx, cy, r - 3, ring, 255);
    circle(c, cx, cy, r - 7, 0x0B0B0B, 255);
    circle(c, cx, cy, r - 11, 0x1A2630, 255);
    circle(c, cx - (r - 11) / 3, cy - (r - 11) / 3, (r - 11) / 3, COLOR_WHITE, 70);
}

/** M6：略带俯视的微缩产品图，保留顶盖拨盘、测距窗、红点与多层镜头。 */
static void draw_m6_art(gfx_canvas_t *c, int cx, int cy)
{
    if (img_picker_m6.pixels) {
        gfx_blit(c, &img_picker_m6, cx - img_picker_m6.width / 2, cy - img_picker_m6.height / 2, 255);
        return;
    }
    const int bx = cx - ART_W / 2;
    const int by = cy - ART_H / 2;
    gfx_shadow(c, gfx_rect(bx + 3, by + 18, ART_W - 6, ART_H - 14), 12, 12, COLOR_BLACK, 165);

    /* 右侧收进去一点，形成三分之四视角。 */
    quad(c, bx + 8, by + 18, bx + ART_W - 16, by + 18, bx + ART_W - 5, by + 31, bx + 2, by + 31, 0xD0CDC5,
         255);
    gfx_tile(c, &img_tex_alu, gfx_rect(bx + 2, by + 27, ART_W - 8, 31), bx, by, 8);
    gfx_tile(c, &img_tex_vulc, gfx_rect(bx + 2, by + 58, ART_W - 8, 39), bx, by, 0);
    gfx_tile(c, &img_tex_alu, gfx_rect(bx + 5, by + 97, ART_W - 14, 8), bx, by, 3);
    quad(c, bx + ART_W - 6, by + 31, bx + ART_W + 1, by + 36, bx + ART_W - 3, by + 97,
         bx + ART_W - 10, by + 97, 0x77766F, 255);
    gfx_fill(c, gfx_rect(bx + 12, by + 28, ART_W - 32, 1), COLOR_WHITE, 145);
    gfx_fill(c, gfx_rect(bx + 2, by + 57, ART_W - 8, 1), COLOR_BLACK, 105);

    /* 顶盖拨盘、快门和卷片杆。 */
    circle(c, bx + 29, by + 17, 9, 0xB9B6AE, 255);
    circle(c, bx + 29, by + 17, 5, 0xE2DFD7, 255);
    circle(c, bx + 126, by + 16, 10, 0xAAA79F, 255);
    circle(c, bx + 126, by + 16, 6, 0xD7D3CA, 255);
    gfx_fill_round(c, gfx_rect(bx + 103, by + 12, 13, 5), 2, 0xE0DDD5, 255);

    /* 测距窗组与红点。 */
    gfx_fill_round(c, gfx_rect(bx + 18, by + 35, 25, 14), 2, 0x9FB0B6, 255);
    gfx_stroke_round(c, gfx_rect(bx + 18, by + 35, 25, 14), 2, 1, 0x252523, 255);
    gfx_fill_round(c, gfx_rect(bx + 50, by + 36, 13, 11), 1, 0xE8E9E3, 255);
    gfx_stroke_round(c, gfx_rect(bx + 50, by + 36, 13, 11), 1, 1, 0x3A3A37, 255);
    gfx_fill_round(c, gfx_rect(bx + 119, by + 34, 25, 16), 2, 0x29343C, 255);
    gfx_stroke_round(c, gfx_rect(bx + 119, by + 34, 25, 16), 2, 1, 0x20201F, 255);
    circle(c, bx + 100, by + 42, 4, COLOR_LED, 255);
    draw_lens(c, cx - 3, by + 74, 31, 0xBDB9B0);
}

/** SX-70：按真机展开比例绘制宽低机身、风琴腔、三角支撑、前面板和前伸出片舱。 */
static void draw_sx_art(gfx_canvas_t *c, int cx, int cy)
{
    if (img_picker_sx70.pixels) {
        gfx_blit(c, &img_picker_sx70, cx - img_picker_sx70.width / 2, cy - img_picker_sx70.height / 2, 255);
        return;
    }
    const int bx = cx - ART_W / 2;
    const int by = cy - ART_H / 2;
    gfx_shadow(c, gfx_rect(bx + 4, by + 37, ART_W - 4, 78), 8, 12, COLOR_BLACK, 170);

    /* 右侧风琴腔与银色三角支撑。 */
    quad(c, bx + 95, by + 29, bx + 145, by + 43, bx + 158, by + 94, bx + 116, by + 80, 0x111313, 255);
    for (int k = 0; k < 4; ++k) {
        quad(c, bx + 106 + k * 9, by + 38 + k * 2, bx + 113 + k * 9, by + 40 + k * 2,
             bx + 129 + k * 7, by + 83 + k * 2, bx + 123 + k * 7, by + 81 + k * 2, 0x4A4B49, 125);
    }
    quad(c, bx + 145, by + 40, bx + 151, by + 43, bx + 164, by + 99, bx + 158, by + 96, 0xC6C3BC, 255);

    /* 下层机身、前伸黑色出片舱。 */
    quad(c, bx + 11, by + 51, bx + 139, by + 42, bx + 153, by + 93, bx + 4, by + 96, 0xBEBBB4, 255);
    gfx_fill_round(c, gfx_rect(bx + 9, by + 50, 126, 42), 5, 0xC9C6BE, 255);
    gfx_fill(c, gfx_rect(bx + 15, by + 51, 112, 1), COLOR_WHITE, 145);
    quad(c, bx + 3, by + 84, bx + 135, by + 81, bx + 153, by + 108, bx + 2, by + 111, 0x080909, 255);
    quad(c, bx + 10, by + 91, bx + 132, by + 89, bx + 140, by + 101, bx + 9, by + 103, 0x181A1A, 255);
    gfx_fill(c, gfx_rect(bx + 18, by + 107, 126, 4), 0xB8B5AE, 255);

    /* 棕色甲板、黑色风琴腔与抬起的取景罩。 */
    quad(c, bx + 42, by + 32, bx + 119, by + 27, bx + 132, by + 49, bx + 31, by + 54, 0xB76135, 255);
    quad(c, bx + 57, by + 9, bx + 119, by + 11, bx + 110, by + 43, bx + 64, by + 43, 0x111313, 255);
    quad(c, bx + 48, by + 1, bx + 127, by + 4, bx + 119, by + 18, bx + 42, by + 14, 0xCAC7C0, 255);
    quad(c, bx + 53, by + 3, bx + 122, by + 6, bx + 116, by + 14, bx + 48, by + 11, 0xB76135, 255);
    gfx_fill(c, gfx_rect(bx + 58, by + 17, 56, 2), COLOR_BLACK, 110);

    /* 真机前面板：左红快门、中主镜头、右圆形测光窗。 */
    circle(c, bx + 29, by + 69, 10, 0x690E08, 255);
    circle(c, bx + 29, by + 69, 8, COLOR_LED, 255);
    circle(c, bx + 27, by + 67, 3, COLOR_WHITE, 95);
    draw_lens(c, bx + 72, by + 68, 21, 0xCFCBC2);
    circle(c, bx + 111, by + 68, 14, 0x282A2B, 255);
    circle(c, bx + 111, by + 68, 10, 0xBDBAB2, 255);
    circle(c, bx + 111, by + 68, 7, 0x26313A, 255);
}

/* ---------------------------------------------------------------- 绘制 */

static void draw_card(film_app_t *app, gfx_canvas_t *c, int i)
{
    const cam_state_t *cam = &app->cam;
    const body_info_t *b = &k_bodies[i];
    const bool current = b->instant == (app->settings.instant != 0);
    const bool pressed = cam->picker_pressed == i;
    const gfx_rect_t r = card_rect(cam, i);

    gfx_fill_round(c, r, CARD_RADIUS, pressed ? 0x2C2B28 : 0x1E1D1B, 255);

    const int art_cy = r.y + r.h / 2;
    if (b->instant) {
        draw_sx_art(c, r.x + r.w / 2, art_cy);
    } else {
        draw_m6_art(c, r.x + r.w / 2, art_cy);
    }
    if (pressed) {
        gfx_fill_round(c, r, CARD_RADIUS, COLOR_WHITE, 16);
    }
    if (current) {
        gfx_stroke_round(c, r, CARD_RADIUS, 1, COLOR_AMBER, 245);
        circle(c, r.x + 10, r.y + r.h - 10, 3, COLOR_AMBER, 255);
        const gfx_text_style_t tag =
            ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.2f, COLOR_AMBER, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
        gfx_text(c, &tag, r.x + 20, r.y + r.h - 8, "IN USE");
    } else {
        gfx_stroke_round(c, r, CARD_RADIUS, 1, 0x3B3A37, 255);
    }

    const gfx_text_style_t name = ui_style(&font_jost_m14, 14, 0.08f, current ? 0xF6F1E4 : COLOR_CREAM,
                                           current ? 255 : 210, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &name, r.x + r.w / 2, r.y + r.h + 15, b->name);
    const gfx_text_style_t sub =
        ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.2f, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &sub, r.x + r.w / 2, r.y + r.h + 32, b->sub);
}

void cam_picker_render(film_app_t *app, gfx_canvas_t *c)
{
    const cam_state_t *cam = &app->cam;
    /* 顶边的小提示条：告诉用户这里可以往下拉 */
    if (cam->overlay == CAM_OVL_NONE && cam->shot == SHOT_IDLE && !cam->skin_swapping) {
        gfx_fill_round(c, gfx_rect((SCREEN_W - HINT_W) / 2, HINT_Y + 1, HINT_W, HINT_H), HINT_H / 2, COLOR_BLACK, 90);
        gfx_fill_round(c, gfx_rect((SCREEN_W - HINT_W) / 2, HINT_Y, HINT_W, HINT_H), HINT_H / 2, COLOR_CREAM, 150);
    }
    const float p = cam->picker_pull;
    if (p <= PICKER_VISIBLE_PULL) {
        return;
    }
    /* 只压暗面板下方露出的取景页；面板盖住的部分本来就没画 */
    const int covered = cam_picker_covered_rows(cam);
    gfx_dim(c, gfx_rect(0, covered, SCREEN_W, SCREEN_H - covered), 256 - (int)lroundf(PICKER_DIM_MAX * p));

    /* 面板：上方两个圆角伸到屏幕外，只露出下面两个 */
    const int top = sheet_top(cam);
    const gfx_rect_t sheet = gfx_rect(0, top - PICKER_RADIUS, SCREEN_W, PICKER_SHEET_H + PICKER_RADIUS);
    gfx_gradient_v(c, gfx_rect(0, top + PICKER_SHEET_H, SCREEN_W, PICKER_SHADOW_H), 0, COLOR_BLACK,
                   (uint8_t)lroundf(PICKER_SHADOW_ALPHA * p), COLOR_BLACK, 0);
    gfx_tile(c, &img_tex_vulc, sheet, 0, 0, PICKER_RADIUS);
    gfx_gradient_v(c, gfx_rect(0, top, SCREEN_W, 90), 0, COLOR_BLACK, 120, COLOR_BLACK, 0);
    gfx_fill(c, gfx_rect(PICKER_RADIUS, top + PICKER_SHEET_H - 1, SCREEN_W - 2 * PICKER_RADIUS, 1), COLOR_WHITE, 26);

    const gfx_text_style_t title =
        ui_style(&font_jost_m18, 18, 0.12f, COLOR_CREAM, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &title, SCREEN_W / 2, top + 28, "CAMERAS");
    const gfx_text_style_t hint =
        ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.16f, COLOR_MUTED, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &hint, SCREEN_W / 2, top + 44, "TAP A BODY TO LOAD IT");
    for (int i = 0; i < 2; ++i) {
        draw_card(app, c, i);
    }
    /* 底部把手：向上推收起 */
    gfx_fill_round(c, gfx_rect((SCREEN_W - 64) / 2, top + PICKER_SHEET_H - 22, 64, 3), 2, COLOR_CREAM, 190);
    gfx_fill_round(c, gfx_rect((SCREEN_W - 42) / 2, top + PICKER_SHEET_H - 12, 42, 4), 2, COLOR_BLACK, 130);
}

/* ---------------------------------------------------------------- 状态与手势 */

void cam_picker_reset(film_app_t *app)
{
    cam_state_t *cam = &app->cam;
    cam->picker_pull = 0.0f;
    cam->picker_open = false;
    cam->picker_dragging = false;
    cam->picker_pressed = -1;
    if (cam->overlay == CAM_OVL_PICKER) {
        cam_close(app);
    }
}

bool cam_picker_step(film_app_t *app, uint32_t dt_ms)
{
    cam_state_t *cam = &app->cam;
    if (cam->picker_dragging) {
        return true;
    }
    const float target = cam->overlay == CAM_OVL_PICKER && cam->picker_open ? 1.0f : 0.0f;
    if (fabsf(cam->picker_pull - target) < 0.003f) {
        const bool changed = cam->picker_pull != target;
        cam->picker_pull = target;
        if (target == 0.0f && cam->overlay == CAM_OVL_PICKER) {
            cam_close(app);   /* 收完了才真正关掉弹出层 */
        }
        return changed;
    }
    cam->picker_pull += (target - cam->picker_pull) * fminf(1.0f, (float)dt_ms / PICKER_ANIM_MS);
    return true;
}

static void set_pull_from_drag(cam_state_t *cam, const gesture_t *g)
{
    const float p = cam->picker_drag_from + (float)g->dy / PICKER_SHEET_H;
    cam->picker_pull = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

/** 关闭状态下：从顶边向下拖就开始拉出面板 */
static bool try_begin_pull(film_app_t *app, const gesture_t *g)
{
    cam_state_t *cam = &app->cam;
    if (g->kind != GEST_DRAG_BEGIN || cam->overlay != CAM_OVL_NONE || cam->shot != SHOT_IDLE ||
        g->y0 >= PICKER_EDGE_PX || g->dy <= 0 || g->dy < abs(g->dx)) {
        return false;
    }
    cam_open(app, CAM_OVL_PICKER);
    cam->picker_open = false;
    cam->picker_dragging = true;
    cam->picker_drag_from = 0.0f;
    cam->picker_pressed = -1;
    cam->shutter_down = cam->window_down = cam->counter_down = cam->pack_down = false;
    set_pull_from_drag(cam, g);
    return true;
}

bool cam_picker_gesture(film_app_t *app, const gesture_t *g)
{
    cam_state_t *cam = &app->cam;
    if (cam->overlay != CAM_OVL_PICKER) {
        return try_begin_pull(app, g);
    }
    switch (g->kind) {
    case GEST_PRESS:
        cam->picker_pressed = cam->picker_pull > 0.95f ? (int8_t)card_at(cam, g->x, g->y) : -1;
        break;
    case GEST_DRAG_BEGIN:
        cam->picker_dragging = true;
        cam->picker_drag_from = cam->picker_pull;
        cam->picker_pressed = -1;
        set_pull_from_drag(cam, g);
        break;
    case GEST_DRAG:
        if (cam->picker_dragging) {
            set_pull_from_drag(cam, g);
        }
        break;
    case GEST_DRAG_END:
        if (cam->picker_dragging) {
            cam->picker_dragging = false;
            if (g->vy > PICKER_FLING_PX_S) {
                cam->picker_open = true;
            } else if (g->vy < -PICKER_FLING_PX_S) {
                cam->picker_open = false;
            } else {
                cam->picker_open = cam->picker_pull > PICKER_OPEN_RATIO;
            }
            if (cam->picker_open && !cam->picker_drag_from) {
                app_feedback(app, FILM_FEEDBACK_CLICK);
            }
        }
        break;
    case GEST_TAP: {
        const int card = card_at(cam, g->x, g->y);
        if (card >= 0 && card == cam->picker_pressed) {
            cam->picker_open = false;
            cam->picker_pressed = -1;
            if (k_bodies[card].instant == (app->settings.instant != 0)) {
                app_feedback(app, FILM_FEEDBACK_CLICK);
            } else {
                cam_switch_body(app, k_bodies[card].instant);   /* 内部会关掉弹出层，面板随后收起 */
            }
        } else if (g->y >= sheet_top(cam) + PICKER_SHEET_H) {
            cam->picker_open = false;
        }
        break;
    }
    case GEST_RELEASE:
        cam->picker_pressed = -1;
        break;
    default:
        break;
    }
    return true;
}

/*
 * 设置页：从取景页下拉的机身面板里的 SETTINGS 行进入。
 *
 *   跳过开机动画 / 声音 / 振动 / 胶卷取景 / 关屏时长，再加一张"关于"卡片（固件版本、相机、剩余空间）。
 * 每行点一下即切换并立即保存；开关类用与取景页弹出面板相同的拨杆。页面没有取景，
 * 打开期间平台事件仍转给取景页，冲洗进度、保存结果不会丢。
 */
#include <stdio.h>
#include <string.h>

#include "app_private.h"

/* 布局（屏幕坐标）：5 行设置 + 1 张关于卡片 */
#define ROW_X           24
#define ROW_W           (SCREEN_W - 2 * ROW_X)
#define ROW_H           52
#define ROW_PITCH       58
#define ROW_TOP         (HEADER_H + 10)
#define ROW_HIT_SLOP    3           /*!< 行间 6 像素的缝也算点中相邻行，没有"点空"的死区 */
#define ROW_RADIUS      10
#define ROW_BG          0x1C1B19U
#define ROW_BG_DOWN     0x2A2824U
#define ROW_LINE        0x34322EU
#define LEVER_INSET     40          /*!< 拨杆中心到行右边的距离 */
#define ABOUT_X         36          /*!< 关于卡片收进下方圆角安全区 */
#define ABOUT_W         (SCREEN_W - 2 * ABOUT_X)
#define ABOUT_H         72
#define ABOUT_PAD       12
#define ABOUT_LINE_H    20

typedef enum {
    ROW_SKIP_BOOT = 0,
    ROW_SOUND,
    ROW_HAPTIC,
    ROW_FILM_LOOK,
    ROW_SCREEN_OFF,
} row_id_t;

static gfx_rect_t row_rect(int i)
{
    return gfx_rect(ROW_X, ROW_TOP + i * ROW_PITCH, ROW_W, ROW_H);
}

static gfx_rect_t about_rect(void)
{
    return gfx_rect(ABOUT_X, ROW_TOP + SETTINGS_ROWS * ROW_PITCH, ABOUT_W, ABOUT_H);
}

static int row_at(int x, int y)
{
    for (int i = 0; i < SETTINGS_ROWS; ++i) {
        if (ui_hit(row_rect(i), x, y, ROW_HIT_SLOP)) {
            return i;
        }
    }
    return -1;
}

/** 关屏时长档位的显示文字 */
static const char *timeout_label(uint8_t level)
{
    uint32_t dim_ms, off_ms;
    film_settings_screen_timeout_ms(level, &dim_ms, &off_ms);
    switch (off_ms / 1000) {
    case 30:
        return "30 S";
    case 60:
        return "1 MIN";
    case 120:
        return "2 MIN";
    default:
        return "5 MIN";
    }
}

/* ---------------------------------------------------------------- 绘制 */

typedef struct {
    const char *name;
    const char *sub;
} row_text_t;

static const row_text_t k_rows[SETTINGS_ROWS] = {
    [ROW_SKIP_BOOT] = { "SKIP BOOT ANIMATION", "Go straight to the camera" },
    [ROW_SOUND] = { "SOUND", "Shutter, dial and boot sounds" },
    [ROW_HAPTIC] = { "VIBRATION", "Haptic click on every control" },
    [ROW_FILM_LOOK] = { "FILM VIEWFINDER", "Tone only  \xC2\xB7  no grain or glow" },
    [ROW_SCREEN_OFF] = { "SCREEN OFF", "Dims after half this time" },
};

/** 某行当前是否"开"（开关类） */
static bool row_on(const film_settings_t *s, int i)
{
    switch (i) {
    case ROW_SKIP_BOOT:
        return s->skip_boot != 0;
    case ROW_SOUND:
        return s->sound_off == 0;
    case ROW_HAPTIC:
        return s->haptic_off == 0;
    default:
        return s->film_look != 0;
    }
}

static void draw_row(film_app_t *app, gfx_canvas_t *c, int i)
{
    const gfx_rect_t r = row_rect(i);
    const bool pressed = app->settings_page.pressed == i;
    gfx_fill_round(c, r, ROW_RADIUS, pressed ? ROW_BG_DOWN : ROW_BG, 255);
    gfx_stroke_round(c, r, ROW_RADIUS, 1, ROW_LINE, 255);
    const gfx_text_style_t name = ui_style(&font_jost_m14, UI_TEXT_LINK, 0.08f, COLOR_CREAM, 235, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &name, r.x + 16, r.y + 20, k_rows[i].name);
    const gfx_text_style_t sub =
        ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.14f, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &sub, r.x + 16, r.y + 37, k_rows[i].sub);

    if (i == ROW_SCREEN_OFF) {
        const gfx_text_style_t val =
            ui_style(&font_jost_m14, UI_TEXT_LINK, 0.12f, COLOR_AMBER, 255, GFX_ALIGN_RIGHT, GFX_ROT_0);
        gfx_text(c, &val, r.x + r.w - 18, r.y + r.h / 2, timeout_label(app->settings.screen_timeout));
    } else {
        ui_lever(c, r.x + r.w - LEVER_INSET, r.y + r.h / 2, row_on(&app->settings, i));
    }
}

/** 关于卡片里的一行：左边灰色标签，右边内容 */
static void draw_about_line(gfx_canvas_t *c, const gfx_rect_t *card, int line, const char *label, const char *value)
{
    const int y = card->y + ABOUT_PAD + 8 + line * ABOUT_LINE_H;
    const gfx_text_style_t l =
        ui_style(&font_jost_m10, UI_TEXT_CAPTION, 0.2f, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &l, card->x + ABOUT_PAD, y, label);
    const gfx_text_style_t v =
        ui_style(&font_jost_m11, UI_TEXT_LABEL, 0.1f, COLOR_CREAM, 235, GFX_ALIGN_RIGHT, GFX_ROT_0);
    gfx_text(c, &v, card->x + card->w - ABOUT_PAD, y, value);
}

static void draw_about(film_app_t *app, gfx_canvas_t *c)
{
    const gfx_rect_t r = about_rect();
    gfx_fill_round(c, r, ROW_RADIUS, 0x151413, 255);
    gfx_stroke_round(c, r, ROW_RADIUS, 1, ROW_LINE, 255);
    const film_about_t *about = &app->settings_page.about;
    draw_about_line(c, &r, 0, "FIRMWARE", about->version[0] ? about->version : "-");
    draw_about_line(c, &r, 1, "CAMERA", about->camera[0] ? about->camera : "-");

    char storage[40] = "-";
    const uint64_t free_bytes = app->port->storage_free ? app->port->storage_free(app->port->ctx) : 0;
    if (free_bytes) {
        const double gb = (double)free_bytes / (1024.0 * 1024.0 * 1024.0);
        if (app->shots_left >= 0) {
            snprintf(storage, sizeof(storage), "%.1f GB  \xC2\xB7  ~%d SHOTS", gb, app->shots_left);
        } else {
            snprintf(storage, sizeof(storage), "%.1f GB FREE", gb);
        }
    } else if (about->storage_total) {
        /* 剩余空间查不起（要遍历整个卷），只显示总容量 */
        snprintf(storage, sizeof(storage), "%.1f GB TOTAL",
                 (double)about->storage_total / (1024.0 * 1024.0 * 1024.0));
    }
    draw_about_line(c, &r, 2, "STORAGE", storage);
}

static void settings_render(film_app_t *app, gfx_canvas_t *c)
{
    gfx_fill(c, gfx_rect(0, 0, SCREEN_W, SCREEN_H), COLOR_INK, 255);
    for (int i = 0; i < SETTINGS_ROWS; ++i) {
        draw_row(app, c, i);
    }
    draw_about(app, c);
    ui_header(c, "SETTINGS", "PREFERENCES", NULL, app->settings_page.back_down);
}

/* ---------------------------------------------------------------- 交互 */

/** 切换一行并保存；声音/振动开关立即通知平台，胶卷取景立即换取景参数 */
static void toggle(film_app_t *app, int row)
{
    film_settings_t *s = &app->settings;
    switch (row) {
    case ROW_SKIP_BOOT:
        s->skip_boot = !s->skip_boot;
        break;
    case ROW_SOUND:
        s->sound_off = !s->sound_off;
        app_apply_feedback_mode(app);
        break;
    case ROW_HAPTIC:
        s->haptic_off = !s->haptic_off;
        app_apply_feedback_mode(app);
        break;
    case ROW_FILM_LOOK:
        s->film_look = !s->film_look;
        app_update_preview(app);
        break;
    default:
        s->screen_timeout = (uint8_t)((s->screen_timeout + 1) % FILM_SCREEN_TIMEOUT_LEVELS);
        break;
    }
    app_save_settings(app);
    app_feedback(app, FILM_FEEDBACK_LEVER);   /* 刚关掉的那一项自然不响 */
}

/*
 * 手指抬起时才生效：按下和抬起落在同一行就切换。比"轻点"宽松——手指略有滑动（超过轻点的 12 像素）
 * 或按得久一点，只要没离开这一行，也照样切换；原来用 GEST_TAP，稍微一抖就被当成拖动而无反应。
 */
static void settings_gesture(film_app_t *app, const gesture_t *g)
{
    settings_page_state_t *p = &app->settings_page;
    switch (g->kind) {
    case GEST_PRESS:
        p->back_down = g->y < HEADER_H && ui_back_hit(g->x, g->y);
        p->pressed = p->back_down ? -1 : (int8_t)row_at(g->x, g->y);
        break;
    case GEST_RELEASE:
        if (p->back_down && g->y < HEADER_H && ui_back_hit(g->x, g->y)) {
            app_feedback(app, FILM_FEEDBACK_CLICK);
            app_go(app, SCR_CAMERA);
        } else if (p->pressed >= 0 && p->pressed == row_at(g->x, g->y)) {
            toggle(app, p->pressed);
        }
        p->pressed = -1;
        p->back_down = false;
        break;
    default:
        break;
    }
}

static void settings_enter(film_app_t *app)
{
    settings_page_state_t *p = &app->settings_page;
    p->pressed = -1;
    p->back_down = false;
    memset(&p->about, 0, sizeof(p->about));
    if (app->port->about) {
        app->port->about(app->port->ctx, &p->about);
    }
    /* 取景页暂停取景：这页不画取景帧 */
}

/** 取景页还在等的平台事件（冲洗进度、保存结果）照常交给它 */
static void settings_event(film_app_t *app, const film_event_t *ev)
{
    if (g_screen_camera.event) {
        g_screen_camera.event(app, ev);
    }
}

const screen_ops_t g_screen_settings = {
    .enter = settings_enter,
    .render = settings_render,
    .gesture = settings_gesture,
    .event = settings_event,
    .key = app_key_back_to_camera,
};

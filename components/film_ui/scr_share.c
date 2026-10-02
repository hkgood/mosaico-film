/*
 * 暗房 · 发送到手机。
 *
 * 平台先尝试连已保存的 Wi-Fi（同网：二维码是相册网址）；没有网络或用户点 USE HOTSPOT 时开热点，
 * 分两步：第 1 步二维码是 Wi-Fi 入网码，手机加入后进入第 2 步，二维码换成相册网址。
 * 离开本页即停止分享（热点随之关闭）。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "app_private.h"
#include "qrcodegen.h"

#define CARD_X          30
#define CARD_Y          66
#define CARD_ANGLE      (-1.0f)
#define QR_BOX          176
#define QR_INSET        12
#define QR_INK          0x1A1A19U
#define STEPS_X         252
#define STEPS_Y         86
#define MEDIA_Y         230
#define STATUS_Y        304
#define PULSE_MS        1400
#define TITLE_X         40      /*!< 左上标题，避开左上圆角 */
#define TITLE_Y         40
#define NOTE_X          40
#define NOTE_Y          340     /*!< 热点说明两行（行距 18） */
#define PULSE_DOT_R_Q4  48

/* 底栏：两个按钮各占一半，或一个通栏按钮 */
#define BTN_HALF_W      ((BOTTOM_ROW_RIGHT - BOTTOM_ROW_LEFT - BOTTOM_ROW_GAP) / 2)
static const gfx_rect_t s_btn_left = { BOTTOM_ROW_LEFT, BOTTOM_ROW_Y, BTN_HALF_W, BOTTOM_ROW_H };
static const gfx_rect_t s_btn_right = { BOTTOM_ROW_RIGHT - BTN_HALF_W, BOTTOM_ROW_Y, BTN_HALF_W, BOTTOM_ROW_H };
static const gfx_rect_t s_btn_full = { BOTTOM_ROW_LEFT, BOTTOM_ROW_Y, BOTTOM_ROW_RIGHT - BOTTOM_ROW_LEFT,
                                       BOTTOM_ROW_H };

/** 分享页是隔着手机操作说明后的主动作，使用确认稿的 16 px 常规字重。 */
static void draw_share_button(gfx_canvas_t *c, gfx_rect_t r, const char *label, bool primary, bool pressed,
                              uint32_t ink)
{
    ui_safe_check(r, 10, label);
    if (primary) {
        gfx_shadow(c, gfx_rect(r.x, r.y + 3, r.w, r.h), 10, 8, COLOR_BLACK, 115);
        gfx_stroke_round(c, gfx_rect(r.x - 1, r.y - 1, r.w + 2, r.h + 2), 11, 1, COLOR_BLACK, 128);
        gfx_tile(c, &img_tex_alu, r, r.x + r.w / 2 - 240, r.y + r.h / 2 - 240, 10);
        gfx_gradient_v(c, r, 10, COLOR_WHITE, 40, COLOR_BLACK, 20);
        gfx_fill(c, gfx_rect(r.x + 10, r.y, r.w - 20, 1), COLOR_WHITE, 204);
    } else {
        if (pressed) {
            gfx_fill_round(c, r, 10, ink, 30);
        }
        gfx_stroke_round(c, r, 10, 1, ink, ink == COLOR_LED ? 210 : 71);
    }
    if (pressed) {
        gfx_fill_round(c, r, 10, COLOR_BLACK, primary ? 40 : 18);
    }
    const gfx_text_style_t st =
        ui_style(&font_jost_r16, 16, 0.02f, primary ? COLOR_ENGRAVE : ink, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(c, &st, r.x + r.w / 2, r.y + r.h / 2, label);
}

static bool is_hotspot(film_share_phase_t phase)
{
    return phase == FILM_SHARE_HOTSPOT_JOIN || phase == FILM_SHARE_HOTSPOT_OPEN;
}

/* ---------------------------------------------------------------- 二维码卡片 */

/** Wi-Fi 入网码里 \ ; , : " 需要转义 */
static void append_escaped(char *dst, size_t len, const char *src)
{
    size_t n = strlen(dst);
    for (; *src && n + 2 < len; ++src) {
        if (strchr("\\;,:\"", *src)) {
            dst[n++] = '\\';
        }
        dst[n++] = *src;
    }
    dst[n] = '\0';
}

/** 当前阶段二维码的内容；没有可扫的内容时为空串 */
static void qr_content(const film_share_status_t *st, char *buf, size_t len)
{
    buf[0] = '\0';
    if (st->phase == FILM_SHARE_HOTSPOT_JOIN) {
        snprintf(buf, len, "WIFI:T:WPA;S:");
        append_escaped(buf, len, st->ssid);
        strncat(buf, ";P:", len - strlen(buf) - 1);
        append_escaped(buf, len, st->password);
        strncat(buf, ";;", len - strlen(buf) - 1);
    } else if (st->phase == FILM_SHARE_LAN || st->phase == FILM_SHARE_HOTSPOT_OPEN) {
        snprintf(buf, len, "%s", st->url);
    }
}

static void card_caption(const film_share_status_t *st, char *buf, size_t len)
{
    switch (st->phase) {
    case FILM_SHARE_HOTSPOT_JOIN:
        snprintf(buf, len, "PW · %s", st->password);
        break;
    case FILM_SHARE_LAN:
    case FILM_SHARE_HOTSPOT_OPEN:
        snprintf(buf, len, "%s", st->host);
        break;
    case FILM_SHARE_ERROR:
        snprintf(buf, len, "OFFLINE");
        break;
    default:
        snprintf(buf, len, "CONNECTING");
        break;
    }
}

/** 把相纸、二维码和说明画进离屏卡片，之后每帧只需旋转贴图 */
static void build_card(film_app_t *app)
{
    share_state_t *s = &app->share;
    gfx_canvas_t c;
    gfx_canvas_init(&c, s->card, SHARE_CARD_W, SHARE_CARD_H, SHARE_CARD_W);
    ui_paper(&c, gfx_rect(0, 0, SHARE_CARD_W, SHARE_CARD_H), 0, 0);

    const gfx_rect_t box = gfx_rect(QR_INSET, QR_INSET, QR_BOX, QR_BOX);
    if (s->qr_text[0] &&
        qrcodegen_encodeText(s->qr_text, s->qr_tmp, s->qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                             SHARE_QR_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        /* 整数模块尺寸，边上留 1 个模块的静区，居中放进 176 方框 */
        const int size = qrcodegen_getSize(s->qr);
        const int module = QR_BOX / (size + 2);
        const int x0 = box.x + (QR_BOX - module * size) / 2;
        const int y0 = box.y + (QR_BOX - module * size) / 2;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (qrcodegen_getModule(s->qr, x, y)) {
                    gfx_fill(&c, gfx_rect(x0 + x * module, y0 + y * module, module, module), QR_INK, 255);
                }
            }
        }
    } else {
        /* 还没有可扫的内容：画一个虚位框 */
        gfx_stroke_round(&c, gfx_rect(box.x + 20, box.y + 20, box.w - 40, box.h - 40), 6, 1, QR_INK, 60);
    }
    char caption[FILM_SHARE_TEXT_LEN + 8];
    card_caption(&s->status, caption, sizeof(caption));
    const gfx_text_style_t st = ui_style(&font_jost_m10, 10, 0.16f, 0x2E2D2A, 255, GFX_ALIGN_CENTER, GFX_ROT_0);
    gfx_text(&c, &st, SHARE_CARD_W / 2, 215, caption);
    s->card_valid = true;
}

static void draw_card(film_app_t *app, gfx_canvas_t *c)
{
    share_state_t *s = &app->share;
    if (!s->card_valid) {
        build_card(app);
    }
    gfx_shadow(c, gfx_rect(CARD_X, CARD_Y + 10, SHARE_CARD_W, SHARE_CARD_H), 0, 22, COLOR_BLACK, 140);
    gfx_blit_rotated(c, s->card, NULL, SHARE_CARD_W, SHARE_CARD_H, SHARE_CARD_W, (CARD_X + SHARE_CARD_W / 2) * 16,
                     (CARD_Y + SHARE_CARD_H / 2) * 16, CARD_ANGLE, 1.0f, 255);
}

/* ---------------------------------------------------------------- 右侧说明 */

typedef struct {
    const char *text;
    const char *em;     /*!< 第二行灰字，可为 NULL */
} share_step_t;

static void draw_steps(gfx_canvas_t *c, const share_step_t *steps, int n)
{
    const gfx_text_style_t num = ui_style(&font_jost_m11, 11, 0.04f, COLOR_AMBER, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    const gfx_text_style_t txt = ui_cjk(&font_noto_13, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    const gfx_text_style_t em = ui_cjk(&font_noto_12, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    int y = STEPS_Y;
    for (int i = 0; i < n; ++i) {
        char label[4];
        snprintf(label, sizeof(label), "%02d", i + 1);
        gfx_text(c, &num, STEPS_X, y, label);
        gfx_text(c, &txt, STEPS_X + 30, y, steps[i].text);
        if (steps[i].em && steps[i].em[0]) {
            gfx_text(c, &em, STEPS_X + 30, y + 20, steps[i].em);
            y += 20;
        }
        y += 31;
    }
}

static void format_size(uint32_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024U * 1024U) {
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(buf, len, "%u KB", (unsigned)((bytes + 1023U) / 1024U));
    }
}

/** 把一张缩略图合成成小相纸（3 px 纸边，裁满） */
static void compose_media(film_app_t *app, int i)
{
    share_state_t *s = &app->share;
    const thumb_slot_t *t = app_thumb(app, s->ids[i]);
    if (!t) {
        return;
    }
    gfx_canvas_t c;
    gfx_canvas_init(&c, s->media + i * SHARE_MEDIA_W * SHARE_MEDIA_H, SHARE_MEDIA_W, SHARE_MEDIA_H, SHARE_MEDIA_W);
    ui_paper(&c, gfx_rect(0, 0, SHARE_MEDIA_W, SHARE_MEDIA_H), 0, 0);
    const gfx_rect_t image = gfx_rect(4, 4, SHARE_MEDIA_W - 8, SHARE_MEDIA_H - 11);
    gfx_blit_cover(&c, t->pixels, t->w, t->h, t->w, image, 255);
    gfx_stroke_round(&c, gfx_rect(image.x - 1, image.y - 1, image.w + 2, image.h + 2), 0, 1, 0x625B50, 72);
    s->media_valid[i] = true;
}

static void draw_media(film_app_t *app, gfx_canvas_t *c)
{
    share_state_t *s = &app->share;
    const int n = (int)(s->count < SHARE_STACK_MAX ? s->count : SHARE_STACK_MAX);
    for (int i = 0; i < n; ++i) {
        if (!s->media_valid[i]) {
            compose_media(app, i);
        }
    }
    int text_x;
    if (n == 1) {
        gfx_shadow(c, gfx_rect(STEPS_X, MEDIA_Y + 2, 74, 58), 0, 5, COLOR_BLACK, 128);
        gfx_blit_scaled(c, s->media, SHARE_MEDIA_W, SHARE_MEDIA_H, SHARE_MEDIA_W,
                        gfx_rect(0, 0, SHARE_MEDIA_W, SHARE_MEDIA_H), gfx_rect(STEPS_X, MEDIA_Y - 2, 74, 58), 255);
        text_x = 340;
    } else {
        /* 第一张（最新）放在最上面 */
        for (int i = n - 1; i >= 0; --i) {
            const int k = n - 1 - i;
            const int cx = STEPS_X + k * 7 + SHARE_MEDIA_W / 2;
            const int cy = MEDIA_Y - k * 2 + SHARE_MEDIA_H / 2;
            gfx_shadow(c, gfx_rect(cx - 33, cy - 25, SHARE_MEDIA_W, SHARE_MEDIA_H), 0, 5, COLOR_BLACK, 110);
            gfx_blit_rotated(c, s->media + i * SHARE_MEDIA_W * SHARE_MEDIA_H, NULL, SHARE_MEDIA_W, SHARE_MEDIA_H,
                             SHARE_MEDIA_W, cx * 16, cy * 16, (float)(k - 2) * 3.0f, 1.0f, 255);
        }
        text_x = 356;
    }
    char count[24], size[24];
    snprintf(count, sizeof(count), "%u %s", (unsigned)s->count, s->count == 1 ? "FRAME" : "FRAMES");
    format_size(s->total_bytes, size, sizeof(size));
    const gfx_text_style_t a = ui_style(&font_jost_m11, 10.5f, 0.22f, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &a, text_x, MEDIA_Y + 13, count);
    const gfx_text_style_t b = ui_style(&font_jost_m9, 9, 0.22f, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &b, text_x, MEDIA_Y + 30, size);
}

static const char *status_text(const film_share_status_t *st, char *buf, size_t len)
{
    switch (st->phase) {
    case FILM_SHARE_STARTING:
        return "正在准备网络…";
    case FILM_SHARE_HOTSPOT_JOIN:
        return "等待手机加入";
    case FILM_SHARE_LAN:
    case FILM_SHARE_HOTSPOT_OPEN:
        if (st->downloads) {
            snprintf(buf, len, "手机已保存 %u 张", (unsigned)st->downloads);
            return buf;
        }
        return st->visits ? "手机已打开相册" : "等待手机打开";
    case FILM_SHARE_ERROR:
        return "网络开启失败";
    default:
        return "";
    }
}

/* ---------------------------------------------------------------- 页面 */

static void share_render(film_app_t *app, gfx_canvas_t *c)
{
    share_state_t *s = &app->share;
    const film_share_status_t *st = &s->status;
    gfx_tile(c, &img_tex_vulc, gfx_rect(0, 0, SCREEN_W, SCREEN_H), 0, 0, 0);
    const gfx_text_style_t title = ui_style(&font_jost_m12, 12, 0.26f, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &title, TITLE_X, TITLE_Y, "SEND TO PHONE");
    const char *tag = st->phase == FILM_SHARE_HOTSPOT_JOIN   ? "STEP 1 / 2"
                      : st->phase == FILM_SHARE_HOTSPOT_OPEN ? "STEP 2 / 2"
                      : st->phase == FILM_SHARE_LAN          ? "SAME WI-FI"
                      : st->phase == FILM_SHARE_ERROR        ? "OFFLINE"
                                                             : "";
    const gfx_text_style_t tg =
        ui_style(&font_jost_m9, 9, 0.26f, st->phase == FILM_SHARE_ERROR ? COLOR_LED : COLOR_MUTED, 255,
                 GFX_ALIGN_RIGHT, GFX_ROT_0);
    gfx_text(c, &tg, HEADER_RIGHT_X, TITLE_Y, tag);

    draw_card(app, c);

    share_step_t steps[3];
    int n = 0;
    switch (st->phase) {
    case FILM_SHARE_LAN:
        steps[n++] = (share_step_t) { "手机连上同一个 Wi-Fi", st->ssid };
        steps[n++] = (share_step_t) { "用手机相机扫描二维码", NULL };
        steps[n++] = (share_step_t) { "网页中可保存全部或单张", NULL };
        break;
    case FILM_SHARE_HOTSPOT_JOIN:
        steps[n++] = (share_step_t) { "相机未联网，已开启热点", st->ssid };
        steps[n++] = (share_step_t) { "用手机相机扫码加入热点", NULL };
        steps[n++] = (share_step_t) { "加入后自动进入第 2 步", NULL };
        break;
    case FILM_SHARE_HOTSPOT_OPEN:
        steps[n++] = (share_step_t) { "手机已加入相机热点", st->ssid };
        steps[n++] = (share_step_t) { "再扫一次二维码打开相册", NULL };
        steps[n++] = (share_step_t) { "网页中可保存全部或单张", NULL };
        break;
    case FILM_SHARE_ERROR:
        steps[n++] = (share_step_t) { "无法连接 Wi-Fi 或开启热点", NULL };
        steps[n++] = (share_step_t) { "请稍后重试", NULL };
        break;
    default:
        steps[n++] = (share_step_t) { "正在连接 Wi-Fi…", NULL };
        break;
    }
    draw_steps(c, steps, n);
    draw_media(app, c);

    /* 状态点：琥珀色呼吸 */
    char buf[40];
    const float t = (float)(app->now % PULSE_MS) / PULSE_MS;
    const uint8_t glow = (uint8_t)(90 + 80 * sinf(t * 6.2831853f));
    const uint32_t dot = st->phase == FILM_SHARE_ERROR ? COLOR_LED : COLOR_AMBER;
    gfx_glow(c, STEPS_X + 3, STATUS_Y, 8, dot, glow);
    gfx_circle_q4(c, (STEPS_X + 3) * 16, STATUS_Y * 16, PULSE_DOT_R_Q4, dot, 255);
    const gfx_text_style_t ss = ui_cjk(&font_noto_12, COLOR_CREAM, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
    gfx_text(c, &ss, STEPS_X + 14, STATUS_Y + 5, status_text(st, buf, sizeof(buf)));

    if (is_hotspot(st->phase)) {
        const gfx_text_style_t note = ui_cjk(&font_noto_11, COLOR_MUTED, 255, GFX_ALIGN_LEFT, GFX_ROT_0);
        gfx_text(c, &note, NOTE_X, NOTE_Y, "热点仅在本页开启，离开即关闭。");
        gfx_text(c, &note, NOTE_X, NOTE_Y + 18, "手机提示“此网络无法上网”属正常。");
    }
    if (st->phase == FILM_SHARE_HOTSPOT_JOIN) {
        draw_share_button(c, s_btn_full, "CANCEL", false, false, COLOR_CREAM);
    } else if (st->phase == FILM_SHARE_HOTSPOT_OPEN) {
        draw_share_button(c, s_btn_full, "DONE", true, false, COLOR_ENGRAVE);
    } else {
        const bool retry = st->phase == FILM_SHARE_ERROR;
        draw_share_button(c, s_btn_left, retry ? "RETRY" : "USE HOTSPOT", false, s->button_hotspot_down,
                          retry ? COLOR_LED : COLOR_CREAM);
        draw_share_button(c, s_btn_right, "DONE", true, false, COLOR_ENGRAVE);
    }
}

/** 重新读取平台的分享状态；二维码内容变化时重画卡片 */
static void refresh_status(film_app_t *app)
{
    share_state_t *s = &app->share;
    if (app->port->share_status) {
        app->port->share_status(app->port->ctx, &s->status);
    }
    qr_content(&s->status, s->qr_next, sizeof(s->qr_next));
    if (strcmp(s->qr_next, s->qr_text) != 0) {
        memcpy(s->qr_text, s->qr_next, sizeof(s->qr_text));
        s->card_valid = false;
    }
}

static void start(film_app_t *app, bool force_hotspot)
{
    share_state_t *s = &app->share;
    if (!app->port->share_start) {
        s->status.phase = FILM_SHARE_ERROR;
        s->card_valid = false;
        return;
    }
    const esp_err_t err = app->port->share_start(app->port->ctx, s->ids, s->count, force_hotspot);
    s->started = err == ESP_OK;
    if (err != ESP_OK) {
        s->status.phase = FILM_SHARE_ERROR;
        s->card_valid = false;
        return;
    }
    refresh_status(app);
}

static void leave_to(film_app_t *app)
{
    const screen_id_t to = app->share.return_to;
    if (to == SCR_ALBUM) {
        app->album.selecting = false;
        app->album.n_selected = 0;
    }
    app_go(app, to);
}

static void share_gesture(film_app_t *app, const gesture_t *g)
{
    share_state_t *s = &app->share;
    const film_share_phase_t phase = s->status.phase;
    if (g->kind == GEST_PRESS) {
        s->button_hotspot_down = !is_hotspot(phase) && ui_hit(s_btn_left, g->x, g->y, 4);
        return;
    }
    if (g->kind == GEST_RELEASE) {
        s->button_hotspot_down = false;
        return;
    }
    if (g->kind != GEST_TAP) {
        return;
    }
    if (is_hotspot(phase)) {
        if (ui_hit(s_btn_full, g->x, g->y, 4)) {
            app_feedback(app, FILM_FEEDBACK_CLICK);
            leave_to(app);
        }
        return;
    }
    if (ui_hit(s_btn_right, g->x, g->y, 4)) {
        app_feedback(app, FILM_FEEDBACK_CLICK);
        leave_to(app);
    } else if (ui_hit(s_btn_left, g->x, g->y, 4)) {
        app_feedback(app, FILM_FEEDBACK_CLICK);
        /* RETRY 先按原方式重试；USE HOTSPOT 强制开热点 */
        start(app, phase != FILM_SHARE_ERROR);
    }
}

static void share_event(film_app_t *app, const film_event_t *ev)
{
    if (ev->type == FILM_EVT_SHARE) {
        const film_share_phase_t before = app->share.status.phase;
        refresh_status(app);
        if (app->share.status.phase != before) {
            app->share.card_valid = false;
            if (app->share.status.phase == FILM_SHARE_HOTSPOT_OPEN) {
                app_feedback(app, FILM_FEEDBACK_DETENT);
            }
        }
    }
}

static bool share_step(film_app_t *app, uint32_t dt)
{
    (void)app;
    (void)dt;
    return true;  /* 状态点一直在呼吸 */
}

static void share_enter(film_app_t *app)
{
    share_state_t *s = &app->share;
    memset(&s->status, 0, sizeof(s->status));
    s->status.phase = FILM_SHARE_STARTING;
    s->qr_text[0] = '\0';
    s->card_valid = false;
    s->button_hotspot_down = false;
    memset(s->media_valid, 0, sizeof(s->media_valid));
    s->total_bytes = app_photos_bytes(app, s->ids, s->count);
    start(app, false);
}

static void share_leave(film_app_t *app)
{
    share_state_t *s = &app->share;
    if (s->started && app->port->share_stop) {
        app->port->share_stop(app->port->ctx);
    }
    s->started = false;
}

const screen_ops_t g_screen_share = {
    .enter = share_enter,
    .leave = share_leave,
    .step = share_step,
    .render = share_render,
    .gesture = share_gesture,
    .event = share_event,
};

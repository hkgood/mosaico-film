/*
 * 局部重画：取景页上只换了取景帧时，界面只重画取景框，结果必须和整屏重画一模一样。
 *
 * 做法：整屏画一帧留着 → 换传感器画面出新取景帧 → 检查 film_app_damage 只是取景框 →
 * 在留着的画面上只重画这块，和另画的一整屏逐像素比较。覆盖 M6、从全屏胶卷页回来的 M6、SX-70。
 * 若取景框以外有东西依赖帧内容，或某个绘制在裁剪边界上画得不一样，这里会对不上。
 *
 *   film_damage_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "app_private.h"
#include "cam_private.h"
#include "film_app.h"
#include "film_port_pc.h"

#define FRAME_MS   16
#define SENSOR_W   640
#define SENSOR_H   480
#define PIXELS     (FILM_APP_SCREEN_W * FILM_APP_SCREEN_H)

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static film_app_handle_t s_app;
static film_port_pc_handle_t s_port;
static uint16_t *s_screen;     /*!< 跨帧保留的整屏画面（和设备上 film_shell 的用法一样） */
static uint16_t *s_ref;
static uint8_t *s_sensor;
static uint32_t s_now = 1000;
static const char *s_data_dir;   /*!< 模拟器的数据目录：往里写 accel 文件就是在转动机器 */
static uint32_t s_seed = 1;

static uint32_t wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

/* 推进 ms 毫秒；每帧真等一会儿，PC 平台按真实时间出取景帧 */
static void advance(int ms)
{
    for (int t = 0; t < ms; t += FRAME_MS) {
        const struct timespec ts = { 0, FRAME_MS * 1000000L };
        nanosleep(&ts, NULL);
        s_now += FRAME_MS;
        (void)film_app_step(s_app, s_now);
    }
}

static void render(uint16_t *pixels, gfx_rect_t clip)
{
    gfx_canvas_t c = {
        .pixels = pixels, .width = FILM_APP_SCREEN_W, .height = FILM_APP_SCREEN_H,
        .stride = FILM_APP_SCREEN_W, .clip = clip,
    };
    film_app_render(s_app, &c);
}

static void tap(int x, int y)
{
    film_app_pointer(s_app, x, y, true, s_now);
    advance(FRAME_MS);
    film_app_pointer(s_app, x, y, false, s_now);
    advance(400);
}

/** 换一张随机传感器画面，等出现只换了取景帧的那一步，返回它的 damage */
static gfx_rect_t next_live_frame(void)
{
    for (int i = 0; i < SENSOR_W * SENSOR_H * 3; ++i) {
        s_seed = s_seed * 1664525u + 1013904223u;
        s_sensor[i] = (uint8_t)(s_seed >> 24);
    }
    CHECK(film_port_pc_set_sensor(s_port, s_sensor, SENSOR_W, SENSOR_H) == ESP_OK);
    const uint32_t deadline = wall_ms() + 2000;
    while (wall_ms() < deadline) {
        s_now += FRAME_MS;
        if (film_app_step(s_app, s_now)) {
            return film_app_damage(s_app);
        }
    }
    fprintf(stderr, "no preview frame within 2 s\n");
    exit(1);
}

static void check_live_only(const char *name, gfx_rect_t want)
{
    advance(1500);   /* 等动画、提示都结束 */
    const gfx_rect_t full = gfx_rect(0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H);
    render(s_screen, full);
    for (int round = 0; round < 3; ++round) {
        const gfx_rect_t d = next_live_frame();
        if (d.x != want.x || d.y != want.y || d.w != want.w || d.h != want.h) {
            fprintf(stderr, "%s: damage %d,%d %dx%d, want %d,%d %dx%d\n", name, d.x, d.y, d.w, d.h, want.x, want.y,
                    want.w, want.h);
            exit(1);
        }
        render(s_screen, d);
        render(s_ref, full);
        for (int i = 0; i < PIXELS; ++i) {
            if (s_screen[i] != s_ref[i]) {
                fprintf(stderr, "%s round %d: pixel (%d, %d) differs: %04x != %04x\n", name, round,
                        i % FILM_APP_SCREEN_W, i / FILM_APP_SCREEN_W, s_screen[i], s_ref[i]);
                exit(1);
            }
        }
    }
    printf("%s: live-only redraw matches full redraw\n", name);
}

/** 像真机一样转动：写重力方向，等传感器平滑与稳定时间过去，握持方向才会真正切换 */
static void hold(film_app_t *app, gfx_rot_t rot)
{
    static const char *const gravity[] = { "0 1 0", "-1 0 0", "0 -1 0", "1 0 0" };
    char path[256];
    snprintf(path, sizeof(path), "%s/accel", s_data_dir);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fprintf(f, "%s\n", gravity[rot]);
    fclose(f);
    for (int i = 0; i < 400 && app->rot != rot; ++i) {
        advance(FRAME_MS);
    }
    CHECK(app->rot == rot);
    advance(500);
}

/**
 * 整个界面跟着握持方向转：横拿时画面 = 没转的版式整体旋转；取景框的 damage 是旋转后的位置；
 * 触摸点从屏幕坐标转回用户坐标。与没转时逐像素对比（取景窗口以外），并点一下转过的胶卷窗口。
 */
static void check_rotated_ui(film_app_t *app)
{
    const gfx_rect_t full = gfx_rect(0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H);
    advance(1500);
    uint16_t *base = malloc(sizeof(uint16_t) * PIXELS);   /* 没转的版式（check_live_only 会覆盖 s_ref，所以另存） */
    CHECK(base);
    render(base, full);
    for (int r = GFX_ROT_90; r <= GFX_ROT_270; ++r) {
        hold(app, (gfx_rot_t)r);
        render(s_screen, full);
        const int n = FILM_APP_SCREEN_W;
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                int u, v;
                switch (r) {
                case GFX_ROT_90:  u = y;         v = n - 1 - x; break;
                case GFX_ROT_180: u = n - 1 - x; v = n - 1 - y; break;
                default:          u = n - 1 - y; v = x;         break;
                }
                if (v < M6_PLATE_Y) {
                    continue;                      /* 取景窗口里是实时画面，内容不比 */
                }
                if (s_screen[y * n + x] != base[v * n + u]) {
                    fprintf(stderr, "rot %d: screen (%d,%d) != user (%d,%d)\n", r * 90, x, y, u, v);
                    exit(1);
                }
            }
        }
        check_live_only("m6 rotated", gfx_rect_rotate(gfx_rect(0, 0, SCREEN_W, M6_PLATE_Y), SCREEN_W, (gfx_rot_t)r));
    }
    free(base);
    /* 横拿（90°）点转过的胶卷窗口：屏幕坐标 (n-1-v, u) 对应用户坐标 (u, v) */
    hold(app, GFX_ROT_90);
    tap(FILM_APP_SCREEN_W - 1 - M6_WINDOW_Y, M6_WINDOW_X);
    CHECK(app->screen == SCR_FILM);
    /* 胶卷页也跟着转：用户坐标的返回键 (60,40) 在 90° 下落在屏幕 (n-1-40, 60) */
    tap(FILM_APP_SCREEN_W - 1 - 40, 60);
    CHECK(app->screen == SCR_CAMERA);
    hold(app, GFX_ROT_0);
    advance(500);
}

/** 屏幕上 (x,y) 在 rot 方向下对应的用户坐标 */
static void to_user(int r, int x, int y, int *u, int *v)
{
    const int n = FILM_APP_SCREEN_W;
    switch (r) {
    case GFX_ROT_90:  *u = y;         *v = n - 1 - x; break;
    case GFX_ROT_180: *u = n - 1 - x; *v = n - 1 - y; break;
    default:          *u = n - 1 - y; *v = x;         break;
    }
}

/** 设置、相册、胶卷（风格选择）页整页跟着握持方向转：与没转的版式逐像素一致，画面里没有实时取景 */
static void check_page_rotates(film_app_t *app, screen_id_t screen, const char *name)
{
    const gfx_rect_t full = gfx_rect(0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H);
    hold(app, GFX_ROT_0);
    app_go(app, screen);
    advance(1500);
    uint16_t *base = malloc(sizeof(uint16_t) * PIXELS);
    CHECK(base);
    render(base, full);
    for (int r = GFX_ROT_90; r <= GFX_ROT_270; ++r) {
        hold(app, (gfx_rot_t)r);
        CHECK(app->screen == screen);
        advance(300);
        render(s_screen, full);
        for (int y = 0; y < FILM_APP_SCREEN_H; ++y) {
            for (int x = 0; x < FILM_APP_SCREEN_W; ++x) {
                int u, v;
                to_user(r, x, y, &u, &v);
                if (s_screen[y * FILM_APP_SCREEN_W + x] != base[v * FILM_APP_SCREEN_W + u]) {
                    fprintf(stderr, "%s rot %d: screen (%d,%d) != user (%d,%d)\n", name, r * 90, x, y, u, v);
                    exit(1);
                }
            }
        }
    }
    free(base);
    /* 转着时点返回键（用户坐标 (40,30)）要能回到取景页 */
    int sx = 0, sy = 0;
    for (int y = 0; y < FILM_APP_SCREEN_H && !sx; ++y) {
        for (int x = 0; x < FILM_APP_SCREEN_W; ++x) {
            int u, v;
            to_user(GFX_ROT_270, x, y, &u, &v);
            if (u == 40 && v == 30) {
                sx = x;
                sy = y;
                break;
            }
        }
    }
    tap(sx, sy);
    CHECK(app->screen == SCR_CAMERA);
    hold(app, GFX_ROT_0);
    advance(300);
    printf("%s: rotates with the device\n", name);
}

/** 设置页：按一行只重画这一行（局部重画与整屏重画逐像素一致），保存延迟到停手后 */
static void check_settings_row(film_app_t *app)
{
    const gfx_rect_t full = gfx_rect(0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H);
    app_go(app, SCR_SETTINGS);
    advance(200);
    render(s_screen, full);
    const int cx = FILM_APP_SCREEN_W / 2, cy = HEADER_H + 12 + 66 + 29;   /* 第 2 行中央（行高 58、行距 66） */
    const uint8_t before = app->settings.sound_off;
    for (int phase = 0; phase < 2; ++phase) {     /* 0 按下，1 抬起（切换） */
        film_app_pointer(app, cx, cy, phase == 0, s_now);
        s_now += FRAME_MS;
        CHECK(film_app_step(app, s_now));
        const gfx_rect_t d = film_app_damage(app);
        CHECK(d.w > 0 && d.h > 0 && d.h < 80 && d.w < FILM_APP_SCREEN_W);   /* 一行，而不是整屏 */
        render(s_screen, d);
        render(s_ref, full);
        CHECK(memcmp(s_screen, s_ref, sizeof(uint16_t) * PIXELS) == 0);
    }
    CHECK(app->settings.sound_off != before);
    CHECK(app->settings_dirty);          /* 还没写 NVS */
    advance(1200);
    CHECK(!app->settings_dirty);         /* 停手后才写 */
    /* 手指在行内移动不重画 */
    film_app_pointer(app, cx, cy, true, s_now);
    s_now += FRAME_MS;
    (void)film_app_step(app, s_now);
    film_app_pointer(app, cx + 6, cy + 3, true, s_now);
    s_now += FRAME_MS;
    CHECK(!film_app_step(app, s_now));
    film_app_pointer(app, 0, 400, false, s_now);   /* 抬在别处：取消，不切换 */
    advance(100);

    /* 内容超过一屏：上拖滚动，局部重画与整屏重画逐像素一致；滚到底能看到关于卡片，不会滚过头 */
    const float max = (float)(HEADER_H + 12 + SETTINGS_ROWS * 66 + 3 * 44 + 44 - FILM_APP_SCREEN_H);
    CHECK(app->settings_page.scroll == 0 && max > 0);
    film_app_pointer(app, cx, 380, true, s_now);
    for (int y = 380; y >= 240; y -= 14) {
        s_now += FRAME_MS;
        film_app_pointer(app, cx, y, true, s_now);
        (void)film_app_step(app, s_now);
        const gfx_rect_t d = film_app_damage(app);
        render(s_screen, d);
        render(s_ref, full);
        CHECK(memcmp(s_screen, s_ref, sizeof(uint16_t) * PIXELS) == 0);
    }
    film_app_pointer(app, cx, 240, false, s_now);
    advance(1500);   /* 惯性滚完 */
    CHECK(app->settings_page.scroll > 50.0f && app->settings_page.scroll <= max);
    CHECK(app->settings_page.pressed == -1);
    app_go(app, SCR_CAMERA);
    advance(500);
}

int main(void)
{
    char data_dir[] = "/tmp/film_damage_XXXXXX";
    CHECK(mkdtemp(data_dir));
    s_data_dir = data_dir;
    const film_port_pc_config_t pc = {
        .data_dir = data_dir, .sensor_ppm = FILM_TEST_SENSOR, .assets_path = FILM_TEST_ASSETS,
    };
    CHECK(film_port_pc_create(&pc, &s_port) == ESP_OK);
    s_screen = malloc(sizeof(uint16_t) * PIXELS);
    s_ref = malloc(sizeof(uint16_t) * PIXELS);
    s_sensor = malloc((size_t)SENSOR_W * SENSOR_H * 3);
    CHECK(s_screen && s_ref && s_sensor);
    const film_app_config_t ac = { .port = film_port_pc_table(s_port), .seed = 42 };
    CHECK(film_app_create(&ac, &s_app) == ESP_OK);
    film_app_t *app = s_app;
    CHECK(app->screen == SCR_CAMERA && !app->settings.instant);

    check_live_only("m6", gfx_rect(0, 0, SCREEN_W, M6_PLATE_Y));

    check_rotated_ui(app);
    check_live_only("m6", gfx_rect(0, 0, SCREEN_W, M6_PLATE_Y));
    check_settings_row(app);
    check_page_rotates(app, SCR_SETTINGS, "settings");
    check_page_rotates(app, SCR_ALBUM, "album");
    check_page_rotates(app, SCR_FILM, "film");

    /* 全屏胶卷页不显示取景；返回后取景照常只重画取景框 */
    tap(M6_WINDOW_X, M6_WINDOW_Y);
    CHECK(app->screen == SCR_FILM && app->preview_paused);
    tap(60, 40);     /* 返回键 */
    CHECK(app->screen == SCR_CAMERA);
    check_live_only("m6 after film page", gfx_rect(0, 0, SCREEN_W, M6_PLATE_Y));

    cam_toggle_instant(app);
    advance(2500);   /* 换机身动画 */
    CHECK(app->settings.instant && !app->cam.skin_swapping);
    check_live_only("sx70", gfx_rect(SX_VF_X, 0, FILM_VF_SX_W, FILM_VF_SX_H));

    film_app_delete(s_app);
    film_port_pc_delete(s_port);
    free(s_screen);
    free(s_ref);
    free(s_sensor);
    printf("film_damage_test: OK\n");
    return 0;
}

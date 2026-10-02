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

int main(void)
{
    char data_dir[] = "/tmp/film_damage_XXXXXX";
    CHECK(mkdtemp(data_dir));
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

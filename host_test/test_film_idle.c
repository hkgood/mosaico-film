/*
 * 屏幕休眠：15 s 调暗并停取景、30 s 关屏（只交出一张黑画面）；触摸、按键、拿起唤醒，
 * 唤醒的那一下不交给页面；手指按着或平台忙时不休眠；远程操作唤醒但不吞输入。
 *
 * 用 PC 平台服务驱动完整的 film_app，只替换要观察的几个平台接口；时钟是虚拟的，
 * 几十秒的空闲瞬间跑完。
 *
 *   film_idle_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app_private.h"
#include "film_app.h"
#include "film_port_pc.h"

#define FRAME_MS 16

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* 被替换接口记下的东西（测试单线程，只有界面线程调用它们） */
static struct {
    const film_port_t *pc;      /*!< 其余接口照常转给 PC 平台 */
    film_display_t display;
    int display_calls;
    bool preview_paused;
    bool keep_awake;
    int shots;
    float accel[3];
} g;

static film_app_handle_t s_app;
static uint32_t s_now = 1000;

static void stub_set_display(void *ctx, film_display_t state)
{
    (void)ctx;
    g.display = state;
    ++g.display_calls;
}

static bool stub_keep_awake(void *ctx)
{
    (void)ctx;
    return g.keep_awake;
}

static void stub_preview_pause(void *ctx, bool paused)
{
    g.preview_paused = paused;
    g.pc->preview_pause(ctx, paused);
}

static esp_err_t stub_shoot(void *ctx, const film_shot_t *shot)
{
    (void)ctx;
    (void)shot;
    ++g.shots;
    return ESP_ERR_INVALID_STATE;   /* 只数次数，不真拍：界面当作"暗房忙"处理 */
}

static bool stub_read_accel(void *ctx, float *x, float *y, float *z)
{
    (void)ctx;
    *x = g.accel[0];
    *y = g.accel[1];
    *z = g.accel[2];
    return true;
}

/** 推进 ms 毫秒，返回期间需要重绘的帧数 */
static int advance(int ms)
{
    int redraws = 0;
    for (int t = 0; t < ms; t += FRAME_MS) {
        s_now += FRAME_MS;
        redraws += film_app_step(s_app, s_now) ? 1 : 0;
    }
    return redraws;
}

static void touch(bool pressed)
{
    film_app_pointer(s_app, 240, 240, pressed, s_now);
    advance(FRAME_MS);
}

static void key(bool pressed)
{
    film_app_key(s_app, FILM_KEY_SHUTTER, pressed, s_now);
    advance(FRAME_MS);
}

static void render_check_black(uint16_t *frame, bool want_black)
{
    gfx_canvas_t c = {
        .pixels = frame, .width = FILM_APP_SCREEN_W, .height = FILM_APP_SCREEN_H,
        .stride = FILM_APP_SCREEN_W, .clip = { 0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H },
    };
    memset(frame, 0xFF, sizeof(uint16_t) * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
    film_app_render(s_app, &c);
    bool black = true;
    for (int i = 0; i < FILM_APP_SCREEN_W * FILM_APP_SCREEN_H && black; ++i) {
        black = frame[i] == 0;
    }
    CHECK(black == want_black);
}

/** 从刚有交互开始空等到关屏 */
static void sleep_to_off(void)
{
    advance(IDLE_OFF_MS + 100);
    CHECK(g.display == FILM_DISPLAY_OFF);
}

int main(void)
{
    char data_dir[] = "/tmp/film_idle_XXXXXX";
    CHECK(mkdtemp(data_dir));
    film_port_pc_handle_t port;
    const film_port_pc_config_t pc = {
        .data_dir = data_dir, .sensor_ppm = FILM_TEST_SENSOR, .assets_path = FILM_TEST_ASSETS,
    };
    CHECK(film_port_pc_create(&pc, &port) == ESP_OK);
    g.pc = film_port_pc_table(port);
    film_port_t table = *g.pc;
    table.set_display = stub_set_display;
    table.keep_awake = stub_keep_awake;
    table.preview_pause = stub_preview_pause;
    table.shoot = stub_shoot;
    table.read_accel = stub_read_accel;
    g.accel[1] = 1.0f;   /* 竖直握持 */

    uint16_t *frame = malloc(sizeof(uint16_t) * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
    const film_app_config_t ac = { .port = &table, .seed = 42 };
    CHECK(frame && film_app_create(&ac, &s_app) == ESP_OK);
    film_app_t *app = s_app;

    /* 创建时先通知一次亮屏；取景页在跑 */
    CHECK(g.display_calls == 1 && g.display == FILM_DISPLAY_ON);
    advance(500);
    CHECK(app->screen == SCR_CAMERA && !g.preview_paused);

    /* 15 s 前不动；到点调暗并停取景；30 s 关屏，只交出一张黑画面 */
    advance(IDLE_DIM_MS - 600);
    CHECK(g.display == FILM_DISPLAY_ON && film_app_display(s_app) == FILM_DISPLAY_ON);
    advance(200);
    CHECK(g.display == FILM_DISPLAY_DIM && g.preview_paused);
    render_check_black(frame, false);
    advance(IDLE_OFF_MS - IDLE_DIM_MS - 400);
    CHECK(g.display == FILM_DISPLAY_DIM);
    advance(400);
    CHECK(g.display == FILM_DISPLAY_OFF);
    CHECK(advance(5000) <= 1);
    render_check_black(frame, true);
    CHECK(advance(5000) == 0);

    /* 关屏时按快门键：只唤醒，不拍照；松开后再按才拍 */
    key(true);
    CHECK(g.display == FILM_DISPLAY_ON && g.shots == 0);
    advance(200);
    key(false);
    CHECK(g.shots == 0);
    advance(100);
    CHECK(!g.preview_paused);
    render_check_black(frame, false);
    key(true);
    key(false);
    CHECK(g.shots == 1);
    advance(2000);   /* 等"暗房忙"提示消失 */

    /* 关屏时触摸：从按下到抬起都不交给页面 */
    sleep_to_off();
    touch(true);
    CHECK(g.display == FILM_DISPLAY_ON && !app->touch.down);
    touch(true);
    CHECK(!app->touch.down);
    touch(false);
    touch(true);
    CHECK(app->touch.down);   /* 醒着的下一次触摸照常 */
    touch(false);

    /* 调暗时的零散抬起不会把屏幕弄醒 */
    advance(IDLE_DIM_MS + 100);
    CHECK(g.display == FILM_DISPLAY_DIM);
    film_app_pointer(s_app, 240, 240, false, s_now);
    advance(FRAME_MS);
    CHECK(g.display == FILM_DISPLAY_DIM);

    /* 拿起：重力方向变化超过阈值才唤醒，轻微抖动不算 */
    g.accel[0] = WAKE_MOTION_G * 0.5f;
    advance(1000);
    CHECK(g.display == FILM_DISPLAY_DIM);
    g.accel[0] = WAKE_MOTION_G * 1.6f;
    advance(1000);
    CHECK(g.display == FILM_DISPLAY_ON);

    /* 手指一直按着：不休眠 */
    touch(true);
    advance(IDLE_OFF_MS + 5000);
    CHECK(g.display == FILM_DISPLAY_ON);
    touch(false);
    advance(IDLE_DIM_MS + 100);
    CHECK(g.display == FILM_DISPLAY_DIM);

    /* 平台后台忙（冲洗、写盘）：不休眠；忙完重新计时 */
    touch(true);
    touch(false);
    g.keep_awake = true;
    advance(IDLE_OFF_MS + 5000);
    CHECK(g.display == FILM_DISPLAY_ON);
    g.keep_awake = false;
    advance(IDLE_DIM_MS - 500);
    CHECK(g.display == FILM_DISPLAY_ON);
    advance(1000);
    CHECK(g.display == FILM_DISPLAY_DIM);

    /* 远程操作：唤醒且这一下照常生效 */
    sleep_to_off();
    film_app_wake(s_app, s_now);
    CHECK(g.display == FILM_DISPLAY_ON);
    touch(true);
    CHECK(app->touch.down);
    touch(false);

    /* 删除界面时恢复亮屏 */
    sleep_to_off();
    film_app_delete(s_app);
    CHECK(g.display == FILM_DISPLAY_ON);

    free(frame);
    film_port_pc_delete(port);
    printf("film_idle_test: OK\n");
    return 0;
}

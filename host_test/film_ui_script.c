/*
 * 界面交互脚本：用 PC 平台服务驱动完整的 film_app，按真实触摸序列走一遍主要流程，
 * 每一步存一张画面并检查相册状态。验证的是与模拟器、设备相同的界面代码和交互逻辑。
 *
 *   film_ui_script <数据目录> <截图目录>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_private.h"
#include "film_app.h"
#include "film_library.h"
#include "film_port_pc.h"

#define FRAME_MS 16

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

typedef struct {
    film_app_handle_t app;
    uint16_t *frame;
    const char *data_dir;
    const char *shot_dir;
    const char *root;
    int snap_no;
} script_t;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u);
}

static void frame_tick(script_t *s)
{
    const struct timespec ts = { 0, FRAME_MS * 1000000L };
    nanosleep(&ts, NULL);
    film_app_step(s->app, now_ms());
}

static void wait_ms(script_t *s, int ms)
{
    for (int t = 0; t < ms; t += FRAME_MS) {
        frame_tick(s);
    }
}

static void touch(script_t *s, int x, int y, bool pressed)
{
    film_app_pointer(s->app, x, y, pressed, now_ms());
    frame_tick(s);
}

static void tap(script_t *s, int x, int y)
{
    touch(s, x, y, true);
    touch(s, x, y, true);
    touch(s, x, y, false);
    wait_ms(s, 400);     /* 等过双击判定窗口 */
}

static void long_press(script_t *s, int x, int y)
{
    for (int t = 0; t < 700; t += FRAME_MS) {
        touch(s, x, y, true);
    }
    touch(s, x, y, false);
    wait_ms(s, 300);
}

/** 实体快门键（设备上是红色 AI 键）按下或松开 */
static void key(script_t *s, bool pressed)
{
    film_app_key(s->app, FILM_KEY_SHUTTER, pressed, now_ms());
    frame_tick(s);
}

static void key_click(script_t *s)
{
    key(s, true);
    key(s, false);
    wait_ms(s, 300);
}

static void drag(script_t *s, int x0, int y0, int x1, int y1, int ms)
{
    const int steps = ms / FRAME_MS;
    for (int i = 0; i <= steps; ++i) {
        touch(s, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, true);
    }
    touch(s, x1, y1, false);
    wait_ms(s, 500);
}

static void snap(script_t *s, const char *name)
{
    gfx_canvas_t c = {
        .pixels = s->frame, .width = FILM_APP_SCREEN_W, .height = FILM_APP_SCREEN_H,
        .stride = FILM_APP_SCREEN_W, .clip = { 0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H },
    };
    film_app_render(s->app, &c);
    char path[512];
    snprintf(path, sizeof(path), "%s/%02d_%s.ppm", s->shot_dir, ++s->snap_no, name);
    FILE *f = fopen(path, "wb");
    CHECK(f);
    fprintf(f, "P6\n%d %d\n255\n", FILM_APP_SCREEN_W, FILM_APP_SCREEN_H);
    for (int i = 0; i < FILM_APP_SCREEN_W * FILM_APP_SCREEN_H; ++i) {
        const uint16_t p = s->frame[i];
        const uint8_t rgb[3] = { (uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 63) << 2), (uint8_t)((p & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("snap %s\n", path);
}

/** 模拟转动相机：PC 平台每 500 ms 读一次数据目录里的 accel 文件，横竖切换还要稳定 350 ms */
static void set_gravity(script_t *s, float x, float y)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/accel", s->data_dir);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fprintf(f, "%.2f %.2f 0\n", x, y);
    fclose(f);
    wait_ms(s, 1500);
}

/** 模拟电量：PC 平台每 500 ms 读一次数据目录里的 battery 文件，界面每秒取一次 */
static void set_battery(script_t *s, const char *value)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/battery", s->data_dir);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fprintf(f, "%s\n", value);
    fclose(f);
    wait_ms(s, 1600);
}

/** 只推进不触摸，数 ms 毫秒内有几帧需要重绘 */
static int count_redraws(script_t *s, int ms)
{
    int n = 0;
    for (int t = 0; t < ms; t += FRAME_MS) {
        const struct timespec ts = { 0, FRAME_MS * 1000000L };
        nanosleep(&ts, NULL);
        n += film_app_step(s->app, now_ms()) ? 1 : 0;
    }
    return n;
}

/** 数相册目录里的成片（.SCR 写完才算一张） */
static int count_photos(const char *root)
{
    film_library_handle_t lib;
    const film_library_config_t cfg = { .root = root };
    if (film_library_create(&cfg, &lib) != ESP_OK) {
        return -1;
    }
    const int n = (int)film_library_count(lib);
    film_library_delete(lib);
    return n;
}

static void wait_photos(script_t *s, int want, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 100) {
        if (count_photos(s->root) >= want) {
            return;
        }
        wait_ms(s, 100);
    }
    fprintf(stderr, "timeout waiting for %d photos (have %d)\n", want, count_photos(s->root));
    exit(1);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    const char *data_dir = argv[1];
    char root[512];
    snprintf(root, sizeof(root), "%s/DCIM/MOSAICO", data_dir);
    mkdir(argv[2], 0755);

    /* 每个进度事件后多等一会，让冲洗像真机一样要一两秒，才能截到进度条走到一半的画面 */
    setenv("FILM_SIM_DEVELOP_DELAY_MS", "60", 1);
    film_port_pc_handle_t port;
    const film_port_pc_config_t pc = {
        .data_dir = data_dir, .sensor_ppm = FILM_TEST_SENSOR, .assets_path = FILM_TEST_ASSETS,
    };
    CHECK(film_port_pc_create(&pc, &port) == ESP_OK);
    script_t s = { .data_dir = data_dir, .shot_dir = argv[2], .root = root };
    s.frame = malloc(sizeof(uint16_t) * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
    const film_app_config_t ac = { .port = film_port_pc_table(port), .seed = 42 };
    CHECK(s.frame && film_app_create(&ac, &s.app) == ESP_OK);
    const int start = count_photos(root);
    CHECK(start == 0);   /* 脚本要求空相册 */

    /* M6：取景、换胶卷、调曝光、拨盘、拍照 */
    wait_ms(&s, 600);
    snap(&s, "m6_viewfinder");
    drag(&s, 360, 180, 160, 180, 300);
    snap(&s, "m6_swipe_film");
    drag(&s, 240, 260, 240, 140, 300);
    snap(&s, "m6_ev_up");
    tap(&s, 240, 180);
    tap(&s, 240, 180);   /* 双击回到 EV 0 由两次快速轻点触发；这里只确认不会误触快门 */
    tap(&s, 378, 410);   /* 胶卷窗 */
    wait_ms(&s, 400);
    snap(&s, "m6_dial_open");
    tap(&s, 360, 400);   /* 拨盘右边一格 */
    wait_ms(&s, 300);
    snap(&s, "m6_dial_next");
    tap(&s, 240, 100);   /* 点取景区收起拨盘 */
    wait_ms(&s, 400);
    touch(&s, 240, 414, true);
    touch(&s, 240, 414, false);
    wait_ms(&s, 150);
    snap(&s, "m6_developing");      /* 定格原片 + DEVELOPING（定格至少 260 ms） */
    wait_ms(&s, 900);
    snap(&s, "m6_reveal");          /* 小样已淡入（胶卷效果），进度条还在走 */
    wait_photos(&s, 1, 15000);
    wait_ms(&s, 100);
    snap(&s, "m6_developed");       /* 全尺寸冲完：DEVELOPED，满格 */
    wait_ms(&s, 2500);
    snap(&s, "m6_after_shot");

    /* 电量指示：最宽的 100% 在四个握持方向下都留在圆角安全区内，不压亮框（安全区越界会打 ui-safe-area） */
    CHECK(s.app->battery_valid && s.app->battery.percent == 82);
    set_battery(&s, "100 0");
    CHECK(s.app->battery.percent == 100);
    snap(&s, "m6_battery_full");

    /* 竖拿两个方向与倒拿：刻字、胶卷名与电量跟着转，都要留在圆角安全区内 */
    set_gravity(&s, -1.0f, 0.0f);
    snap(&s, "m6_rot90");
    set_gravity(&s, 1.0f, 0.0f);
    snap(&s, "m6_rot270");
    set_gravity(&s, 0.0f, -1.0f);
    snap(&s, "m6_rot180");
    set_gravity(&s, 0.0f, 1.0f);

    /* 低电量：20% 以下琥珀色；10% 以下红色并只提示一次；充电时显示闪电；读不到时隐藏 */
    set_battery(&s, "18 0");
    CHECK(!s.app->battery_warned && !s.app->toast[0]);
    snap(&s, "battery_low");
    set_battery(&s, "8 0");
    CHECK(s.app->battery_warned && strstr(s.app->toast, "电量低"));
    snap(&s, "battery_critical");
    wait_ms(&s, TOAST_MS + 200);
    set_battery(&s, "7 0");
    CHECK(!s.app->toast[0]);   /* 同一次低电量不重复提示 */
    set_battery(&s, "46 1");
    CHECK(s.app->battery.charging && !s.app->battery_warned);
    snap(&s, "battery_charging");
    set_battery(&s, "none");
    CHECK(!s.app->battery_valid);
    snap(&s, "battery_unknown");
    set_battery(&s, "82 0");

    /* 长按胶卷窗的弹出面板仍在（DATE 开关）；点空白处关掉 */
    long_press(&s, 378, 410);
    snap(&s, "m6_popover");
    tap(&s, 240, 100);
    wait_ms(&s, 300);

    /* 顶边下拉：拉一点松手会收回；拉到底展开机身选择，点 SX-70 */
    drag(&s, 240, 8, 240, 60, 300);
    snap(&s, "picker_cancelled");
    drag(&s, 240, 8, 240, 280, 300);
    snap(&s, "picker_open");
    /* 面板打开期间取景定格、平台暂停帧转换；面板静止时整页不重绘 */
    CHECK(s.app->cam.overlay == CAM_OVL_PICKER && s.app->preview_paused);
    CHECK(count_redraws(&s, 600) == 0);
    tap(&s, 352, 162);
    wait_ms(&s, 1200);
    CHECK(!s.app->preview_paused);
    snap(&s, "sx_body");
    set_gravity(&s, 1.0f, 0.0f);
    snap(&s, "sx_rot270");
    set_gravity(&s, 0.0f, 1.0f);
    drag(&s, 330, 180, 130, 180, 300);
    snap(&s, "sx_swipe_film");
    tap(&s, 396, 416);              /* 胶片盒 → 换胶片抽屉 */
    wait_ms(&s, 400);
    snap(&s, "sx_drawer");
    tap(&s, 240, 200);              /* 点抽屉上方收起 */
    wait_ms(&s, 400);
    tap(&s, 240, 426);
    wait_ms(&s, 600);
    snap(&s, "sx_developing");      /* 相纸吐出，小样在雾层下，进度条 + 百分比 */
    tap(&s, 240, 240);              /* 冲洗中轻点不会跳过 */
    snap(&s, "sx_developing_tap");
    wait_photos(&s, 2, 15000);
    wait_ms(&s, 100);
    snap(&s, "sx_developed");
    tap(&s, 240, 240);              /* 冲完后轻点回到取景 */
    wait_ms(&s, 600);
    snap(&s, "sx_back");

    /* SX-70 也能下拉换回 M6，再换回 SX-70 继续后面的流程 */
    drag(&s, 240, 8, 240, 280, 300);
    snap(&s, "sx_picker_open");
    tap(&s, 128, 162);
    wait_ms(&s, 1200);
    snap(&s, "m6_from_picker");
    drag(&s, 240, 8, 240, 280, 300);
    tap(&s, 352, 162);
    wait_ms(&s, 1200);

    /* 暗房：相册 → 大图 → 重新冲洗 → 另存 */
    tap(&s, 82, 424);               /* SX-70 相纸堆 → 相册 */
    wait_ms(&s, 800);
    snap(&s, "album");
    tap(&s, 80, 67 + 60);
    wait_ms(&s, 800);
    snap(&s, "detail");
    tap(&s, 164, 422);              /* REDEVELOP */
    wait_ms(&s, 3000);
    snap(&s, "redevelop");
    drag(&s, 360, 380, 240, 380, 300);
    wait_ms(&s, 2500);
    snap(&s, "redevelop_other_film");
    tap(&s, 364, 436);              /* DEVELOP */
    wait_photos(&s, 3, 15000);
    wait_ms(&s, 1200);
    snap(&s, "redevelop_saved_detail");

    /* 发送到手机：同网 → 打开 → 保存；再试热点 */
    tap(&s, 344, 422);              /* SEND TO PHONE */
    wait_ms(&s, 1500);
    snap(&s, "share_lan");
    wait_ms(&s, 6500);
    snap(&s, "share_saved");
    tap(&s, 135, 422);              /* USE HOTSPOT */
    wait_ms(&s, 1500);
    snap(&s, "share_hotspot");
    wait_ms(&s, 4500);
    snap(&s, "share_hotspot_open");
    tap(&s, 240, 422);              /* DONE */
    wait_ms(&s, 800);
    snap(&s, "share_done");

    /* 删除：大图 → 垃圾桶 → 确认 */
    tap(&s, 60, 422);               /* 删除 */
    wait_ms(&s, 400);
    snap(&s, "delete_confirm");
    tap(&s, 250 + 60, 246 + 20);
    wait_ms(&s, 800);
    snap(&s, "deleted");
    CHECK(count_photos(root) == 2);

    /* 返回键的点击区比图标大：点在图标右下方（旧的 60×60 之外）也能返回 */
    tap(&s, 86, 70);
    wait_ms(&s, 600);
    snap(&s, "back_to_album");
    tap(&s, 90, 44);
    wait_ms(&s, 800);
    snap(&s, "back_to_camera");

    /* 红色快门键 · SX-70：按下即拍并显示按下态；显影中再按不响应 */
    CHECK(s.app->screen == SCR_CAMERA && s.app->settings.instant && s.app->cam.overlay == CAM_OVL_NONE);
    key(&s, true);
    CHECK(s.app->cam.shutter_down && s.app->cam.shot != SHOT_IDLE);
    snap(&s, "key_sx_pressed");
    key(&s, false);
    CHECK(!s.app->cam.shutter_down);
    wait_ms(&s, 600);
    CHECK(s.app->cam.overlay == CAM_OVL_DEVELOPING);
    key_click(&s);
    CHECK(s.app->cam.overlay == CAM_OVL_DEVELOPING);
    wait_photos(&s, 3, 15000);
    wait_ms(&s, 100);
    tap(&s, 240, 240);              /* 冲完后轻点回到取景 */
    wait_ms(&s, 600);
    CHECK(s.app->cam.overlay == CAM_OVL_NONE);

    /* 弹层开着时第一下只收起，不拍照：SX-70 抽屉、机身选择 */
    tap(&s, 396, 416);
    wait_ms(&s, 400);
    CHECK(s.app->cam.overlay == CAM_OVL_DRAWER);
    key_click(&s);
    CHECK(s.app->cam.overlay == CAM_OVL_NONE && s.app->cam.shot == SHOT_IDLE);
    snap(&s, "key_closed_drawer");
    drag(&s, 240, 8, 240, 280, 300);
    CHECK(s.app->cam.overlay == CAM_OVL_PICKER);
    key_click(&s);
    wait_ms(&s, 600);
    CHECK(s.app->cam.overlay == CAM_OVL_NONE && s.app->cam.shot == SHOT_IDLE && s.app->settings.instant);

    /* 换回 M6：拨盘开着先收起；之后按键拍一张 */
    drag(&s, 240, 8, 240, 280, 300);
    tap(&s, 128, 162);
    wait_ms(&s, 1200);
    CHECK(!s.app->settings.instant);
    tap(&s, 378, 410);
    wait_ms(&s, 400);
    CHECK(s.app->cam.overlay == CAM_OVL_DIAL);
    key_click(&s);
    CHECK(s.app->cam.overlay == CAM_OVL_NONE && s.app->cam.shot == SHOT_IDLE);
    wait_ms(&s, 400);
    key(&s, true);
    CHECK(s.app->cam.shot != SHOT_IDLE);
    snap(&s, "key_m6_shot");
    key(&s, false);
    wait_photos(&s, 4, 15000);
    wait_ms(&s, 2600);
    CHECK(s.app->cam.shot == SHOT_IDLE);

    /* 暗房各页按键回到取景，不拍照 */
    tap(&s, 92, 410);               /* M6 计数窗 → 相册 */
    wait_ms(&s, 600);
    CHECK(s.app->screen == SCR_ALBUM);
    key_click(&s);
    CHECK(s.app->screen == SCR_CAMERA);
    tap(&s, 92, 410);
    wait_ms(&s, 600);
    tap(&s, 80, 67 + 60);
    wait_ms(&s, 800);
    CHECK(s.app->screen == SCR_DETAIL);
    key_click(&s);
    CHECK(s.app->screen == SCR_CAMERA && s.app->cam.shot == SHOT_IDLE);
    wait_ms(&s, 600);
    CHECK(count_photos(root) == 4);

    /* 多选删除：没选时删除键无效；选 3 张 → 删除 → 取消 → 再删除 → 确认 */
    tap(&s, 92, 410);
    wait_ms(&s, 600);
    tap(&s, 440, 32);               /* SELECT */
    CHECK(s.app->album.selecting && s.app->album.n_selected == 0);
    tap(&s, 208, 422);
    CHECK(!s.app->album.confirm_delete);
    tap(&s, 80, 127);
    tap(&s, 240, 127);
    tap(&s, 400, 127);
    CHECK(s.app->album.n_selected == 3);
    snap(&s, "select_three");
    tap(&s, 208, 422);              /* 删除 */
    CHECK(s.app->album.confirm_delete);
    snap(&s, "multi_delete_confirm");
    tap(&s, 170, 266);              /* CANCEL */
    CHECK(!s.app->album.confirm_delete && s.app->album.selecting && s.app->album.n_selected == 3);
    tap(&s, 208, 422);
    tap(&s, 310, 266);              /* DELETE */
    wait_ms(&s, 600);
    CHECK(!s.app->album.selecting && s.app->album.n_selected == 0);
    CHECK(count_photos(root) == 1);
    snap(&s, "multi_deleted");
    key_click(&s);
    wait_ms(&s, 600);
    CHECK(s.app->screen == SCR_CAMERA);
    snap(&s, "end_camera");

    film_app_delete(s.app);
    film_port_pc_delete(port);
    free(s.frame);
    printf("film_ui_script: OK\n");
    return 0;
}

/*
 * film_capture_mode：两块相机板的真实格式表（esp_cam_sensor 驱动里的格式 + 本项目 Kconfig 打开的档）
 * 在设备实测的 PSRAM 余量下选出的格式，以及 4:3 窗口规则与 film_viewfinder_window_43 一致。
 */
#include <stdio.h>
#include <stdlib.h>

#include "film_capture_mode.h"
#include "film_viewfinder.h"

static int failures;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                  \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

#define KB(n) ((size_t)(n) * 1024u)
#define MB(n) ((size_t)(n) * 1024u * 1024u)

/* 设备上开流前的空闲 PSRAM：640×480 取景时实测剩 5279 KB、流占 1802 KB */
static const film_capture_env_t k_device = { .psram_free = KB(5279 + 1802), .psram_largest = KB(5000) };

/* OV3640 驱动的全部格式（Kconfig 全开时的顺序） */
static const film_capture_mode_t k_ov3640[] = {
    { FILM_CAPTURE_PIXEL_OTHER, 640, 480, 7 },      /* RGB565 */
    { FILM_CAPTURE_PIXEL_UYVY, 640, 480, 30 },
    { FILM_CAPTURE_PIXEL_UYVY, 1024, 768, 5 },
    { FILM_CAPTURE_PIXEL_UYVY, 1024, 768, 25 },
    { FILM_CAPTURE_PIXEL_UYVY, 1280, 720, 15 },
    { FILM_CAPTURE_PIXEL_JPEG, 2048, 1536, 15 },
    { FILM_CAPTURE_PIXEL_JPEG, 1024, 768, 25 },
    { FILM_CAPTURE_PIXEL_JPEG, 1280, 720, 15 },
};

/* SC101IOT 驱动的全部格式 */
static const film_capture_mode_t k_sc101iot[] = {
    { FILM_CAPTURE_PIXEL_UYVY, 1280, 720, 15 },
    { FILM_CAPTURE_PIXEL_UYVY, 1280, 720, 25 },
    { FILM_CAPTURE_PIXEL_OTHER, 1280, 720, 15 },    /* YUYV */
    { FILM_CAPTURE_PIXEL_OTHER, 1280, 720, 25 },
    { FILM_CAPTURE_PIXEL_UYVY, 480, 640, 15 },      /* 竖幅 */
    { FILM_CAPTURE_PIXEL_UYVY, 640, 480, 15 },      /* 1280×720 中间裁出的 640×480，视角只有一半 */
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void test_ov3640(void)
{
    film_capture_env_t env = k_device;
    env.jpeg_still = true;
    film_capture_plan_t plan;
    film_capture_mode_plan(k_ov3640, COUNT(k_ov3640), &env, &plan);
    CHECK(plan.preview == 1);       /* 640×480 30 fps：帧率最高、最省 */
    CHECK(plan.buffers == 2);
    CHECK(plan.still == 5);         /* 2048×1536 JPEG */
    CHECK(plan.photo_width == 2048 && plan.photo_height == 1536);

    /* 本项目实际编进去的两档 */
    const film_capture_mode_t built[] = { k_ov3640[1], k_ov3640[5] };
    film_capture_mode_plan(built, COUNT(built), &env, &plan);
    CHECK(plan.preview == 0 && plan.still == 1 && plan.buffers == 2);
}

static void test_sc101iot(void)
{
    film_capture_env_t env = k_device;
    env.jpeg_still = false;
    film_capture_plan_t plan;
    film_capture_mode_plan(k_sc101iot, COUNT(k_sc101iot), &env, &plan);
    /* 1280×720 双缓冲放不下（5.4 MB 流 + 1.3 MB 原片 + 余量），单缓冲可以；15 fps 优先于 25 fps */
    CHECK(plan.preview == 0);
    CHECK(plan.buffers == 1);
    CHECK(plan.still == -1);
    CHECK(plan.photo_width == 960 && plan.photo_height == 720);

    /* PSRAM 充裕时双缓冲 */
    env.psram_free = MB(12);
    film_capture_mode_plan(k_sc101iot, COUNT(k_sc101iot), &env, &plan);
    CHECK(plan.preview == 0 && plan.buffers == 2);

    /* PSRAM 紧张时退到 640×480 */
    env.psram_free = MB(4);
    film_capture_mode_plan(k_sc101iot, COUNT(k_sc101iot), &env, &plan);
    CHECK(plan.preview == 5 && plan.buffers == 2);
    CHECK(plan.photo_width == 640 && plan.photo_height == 480);

    /* 连续块不够一帧 1280×720 时也退到 640×480 */
    env = k_device;
    env.psram_largest = KB(1500);
    film_capture_mode_plan(k_sc101iot, COUNT(k_sc101iot), &env, &plan);
    CHECK(plan.preview == 5);

    /* 竖幅与 YUYV 永远不选 */
    const film_capture_mode_t odd[] = { k_sc101iot[2], k_sc101iot[4] };
    film_capture_mode_plan(odd, COUNT(odd), &k_device, &plan);
    CHECK(plan.preview == -1 && plan.photo_width == 0);
}

static void test_edges(void)
{
    film_capture_plan_t plan;
    film_capture_mode_plan(NULL, 0, &k_device, &plan);
    CHECK(plan.preview == -1 && plan.still == -1 && plan.buffers == 2);

    /* JPEG 传感器但取景放不下：照样用 JPEG 拍照，取景沿用当前格式 */
    film_capture_env_t env = { .jpeg_still = true, .psram_free = KB(100), .psram_largest = KB(100) };
    film_capture_mode_plan(k_ov3640, COUNT(k_ov3640), &env, &plan);
    CHECK(plan.preview == -1 && plan.still == 5 && plan.photo_width == 2048);

    /* 标了 jpeg_still 却没有 4:3 JPEG：按快门帧规则选 */
    env = k_device;
    env.jpeg_still = true;
    film_capture_mode_plan(k_sc101iot, COUNT(k_sc101iot), &env, &plan);
    CHECK(plan.still == -1 && plan.preview == 0);

    /* 流占用 = (缓冲 + 驱动备用 1 块) × 帧 */
    CHECK(film_capture_mode_stream_bytes(&k_ov3640[1], 2) == 3u * 640u * 480u * 2u);
}

/* 4:3 窗口与取景转换用的 film_viewfinder_window_43 一致 */
static void test_window_matches_viewfinder(void)
{
    static uint8_t dummy[4];
    const uint16_t sizes[][2] = { { 1280, 720 }, { 640, 480 }, { 480, 640 }, { 1920, 1080 }, { 2048, 1536 },
                                  { 1000, 500 }, { 642, 480 }, { 800, 600 } };
    for (size_t i = 0; i < COUNT(sizes); ++i) {
        uint16_t w, h;
        film_capture_mode_window(sizes[i][0], sizes[i][1], &w, &h);
        const film_viewfinder_source_t src = { .uyvy = dummy, .width = sizes[i][0], .height = sizes[i][1] };
        const film_viewfinder_source_t win = film_viewfinder_window_43(&src);
        CHECK(win.width == w && win.height == h);
    }
}

int main(void)
{
    test_ov3640();
    test_sc101iot();
    test_edges();
    test_window_matches_viewfinder();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return EXIT_FAILURE;
    }
    printf("film_capture_mode_test: ok\n");
    return EXIT_SUCCESS;
}

// SPDX-License-Identifier: Apache-2.0
/*
 * Mosaico Film 设备入口。
 *
 * 启动顺序（先让屏幕亮起来，再做慢的初始化）：
 *   1. ESP-Iris / OTA 支持（保证 Vibe Mode 始终可达）
 *   2. 映射素材分区；软复位时给外设电源轨断一次电（见 reset_peripheral_rail）
 *      然后合成声音、点亮屏幕、开始播开机动画
 *   3. 动画播放的同时初始化相机、NAND、暗房、Wi-Fi、IMU
 *   4. 动画播完且服务就绪后进入相机界面
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

#define GSP_BUNDLE_ENABLE_RAW_IDS 1     /* Canvas 接口按 bind 编号寻址 */
#include "board_display.h"
#include "bsp/esp_mosaico.h"
#include "bundle_gsp.h"
#include "esp_check.h"
#include "esp_gsp_esp_lcd.h"
#include "esp_heap_caps.h"
#include "esp_iris.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "film_assets.h"
#include "film_boot.h"
#include "film_feedback.h"
#include "film_port_dev.h"
#include "film_remote_input.h"
#include "film_shell.h"
#include "film_shutter_key.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iris_ota_support.h"
#include "iris_screen_mirror.h"
#include "nvs_flash.h"
#include "ui_bundle.h"

static const char *TAG = "mosaico_film";

#define ASSETS_PARTITION_SUBTYPE 0x41
#define ASSETS_PARTITION_LABEL   "assets"
#define LOCAL_TIMEZONE           "CST-8"
#define FEEDBACK_VOLUME          70
#define FEEDBACK_CORE            1
#define FRAME_ALIGN              64
#define FRAME_CAPS               (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define RAIL_OFF_MS              300     /*!< 软复位后 VCC_3V3 断电多久，让外设彻底掉电 */

/*
 * 启动期的单一所有者：app_main 写入；GSP 渲染任务在 intro 回调里读取 s_port。
 * s_port 用原子量发布，渲染任务看到非 NULL 时其余服务都已就绪。
 */
static film_boot_handle_t s_boot;
static _Atomic(const film_port_t *) s_port;
static film_shell_handle_t s_shell;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void report_render_stats(const film_shell_stats_t *stats)
{
    ESP_LOGI(TAG, "render: %" PRIu32 " frames / %" PRIu32 " ms, avg %" PRIu32 " us, max %" PRIu32 " us",
             stats->frames, stats->period_ms, stats->avg_us, stats->max_us);
}

static void *frame_alloc(size_t bytes)
{
    return heap_caps_aligned_calloc(FRAME_ALIGN, 1, bytes, FRAME_CAPS);
}

/* 资源分区（字体、贴图、开机动画）映射进地址空间后整个运行期都不解除 */
static esp_err_t map_assets(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ASSETS_PARTITION_SUBTYPE, ASSETS_PARTITION_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "assets partition missing");
        return ESP_ERR_NOT_FOUND;
    }
    const void *data = NULL;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, FILM_ASSETS_SIZE, ESP_PARTITION_MMAP_DATA, &data, &handle);
    if (err == ESP_OK) {
        err = film_assets_bind(data, FILM_ASSETS_SIZE);
        if (err != ESP_OK) {
            esp_partition_munmap(handle);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "assets unavailable: %s (run iris system-update)", esp_err_to_name(err));
    }
    return err;
}

/*
 * 软复位（OTA 重启、进出 Vibe Mode）不会给 VCC_3V3 上的外设断电：相机板、NAND、屏幕会带着
 * 上一次运行的状态进来。真机上见过这种情况下传感器 SCCB 正常却一帧不出、NAND 读回乱码，
 * 只有物理断电能恢复。所以非上电复位时先把这条轨断开一会儿，再由各驱动按需重新上电。
 * 必须在任何外设（音频、屏幕、NAND、相机）初始化之前调用。
 */
static void reset_peripheral_rail(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    if (reason == ESP_RST_POWERON) {
        return;
    }
    esp_err_t err = bsp_power_set_vcc_3v3(false);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(RAIL_OFF_MS));
        err = bsp_power_set_vcc_3v3(true);
    }
    ESP_LOGI(TAG, "reset reason %d: VCC_3V3 power-cycled (%s)", (int)reason, esp_err_to_name(err));
}

/* ---------------------------------------------------------------- 开机动画（GSP 渲染任务） */

static bool intro_frame(void *ctx, uint32_t t_ms, uint16_t *pixels)
{
    (void)ctx;
    return s_boot && film_boot_frame(s_boot, t_ms, pixels);
}

static const film_port_t *intro_port(void *ctx)
{
    (void)ctx;
    const film_port_t *port = atomic_load(&s_port);
    if (port && s_boot) {
        film_boot_delete(s_boot);   /* 动画不会再播：释放解码器与缓冲 */
        s_boot = NULL;
    }
    return port;
}

static const film_shell_intro_t s_intro = { .frame = intro_frame, .port = intro_port };

/* ---------------------------------------------------------------- 入口 */

static esp_err_t start_ui(film_feedback_handle_t feedback)
{
    esp_gsp_config_t app_config;
    esp_err_t err = ui_bundle_open(&app_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GSP UI unavailable (0x%x); ESP-Iris Recovery RPC remains active", err);
        return err;
    }
    esp_display_present_target_config_t display;
    ESP_RETURN_ON_ERROR(board_display_init(&display), TAG, "display");
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(board_touch_init(&touch), TAG, "touch");
    esp_gsp_esp_lcd_config_t lcd = ESP_GSP_ESP_LCD_CONFIG_INIT();
    lcd.display = display;
    lcd.touch = touch;
    ESP_RETURN_ON_ERROR(iris_screen_mirror_init(), TAG, "screen mirror");
    esp_gsp_handle_t ui;
    ESP_RETURN_ON_ERROR(esp_gsp_esp_lcd_start(&app_config, &lcd, &ui), TAG, "GSP start");
    ESP_RETURN_ON_ERROR(iris_screen_mirror_attach(ui), TAG, "mirror attach");

    const uint8_t *anim = NULL;
    size_t anim_size = 0;
    if (film_assets_boot_anim(&anim, &anim_size) && film_boot_create(anim, anim_size, feedback, &s_boot) != ESP_OK) {
        s_boot = NULL;   /* 没有动画就直接停在黑屏，等服务就绪 */
    }
    const film_shell_config_t shell = {
        .canvas_bind = GSP_FILM_BIND_SCREEN,
        .intro = &s_intro,
        .seed = esp_random() | 1u,
        .now_ms = now_ms,
        .alloc = frame_alloc,
        .free = heap_caps_free,
        .now_us = esp_timer_get_time,
        .report = report_render_stats,
    };
    if (film_shell_start(ui, &shell, &s_shell) != ESP_GSP_OK) {
        return ESP_FAIL;
    }
    if (film_remote_input_start(s_shell) != ESP_OK) {
        ESP_LOGW(TAG, "remote touch unavailable");   /* 只影响 Gateway 远程点按 */
    }
    if (film_shutter_key_start(s_shell) != ESP_OK) {
        ESP_LOGW(TAG, "shutter key unavailable");    /* 触摸快门照常可用 */
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_iris_boot_probe());
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Keep Recovery reachable even when the external UI image is absent or invalid. */
    iris_ota_support_start();

    setenv("TZ", LOCAL_TIMEZONE, 1);
    tzset();

    if (map_assets() != ESP_OK) {
        return;   /* 没有字体就画不出界面；Vibe Mode 仍然可用来修复 */
    }
    reset_peripheral_rail();
    film_feedback_handle_t feedback = NULL;
    const film_feedback_config_t fb = { .volume_percent = FEEDBACK_VOLUME, .core = FEEDBACK_CORE };
    if (film_feedback_create(&fb, &feedback) != ESP_OK) {
        ESP_LOGW(TAG, "sound and vibration unavailable");
    }
    if (start_ui(feedback) != ESP_OK) {
        return;
    }

    /* 慢的初始化放在动画播放期间 */
    const int64_t t0 = esp_timer_get_time();
    film_port_dev_handle_t port = NULL;
    const film_port_dev_config_t port_config = { .feedback = feedback };
    const esp_err_t err = film_port_dev_create(&port_config, &port);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "device services failed: %s", esp_err_to_name(err));
        return;
    }
    atomic_store(&s_port, film_port_dev_table(port));
    ESP_LOGI(TAG, "services ready in %lld ms, PSRAM free %u / %u KB, internal free %u KB",
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

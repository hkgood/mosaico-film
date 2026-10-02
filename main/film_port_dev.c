#include "film_port_dev.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "film_camera.h"
#include "film_lab.h"
#include "film_power.h"
#include "film_share.h"
#include "film_storage.h"
#include "nvs.h"

static const char *TAG = "film_port";

#define EVENT_QUEUE_LEN     16
#define CAMERA_CORE         0
#define CAMERA_PRIORITY     5
/*
 * 暗房放在核 0：GSP 渲染任务固定在核 1，冲洗时取景已停，核 0 基本空闲；
 * 滤镜的并行工作任务会自动落在另一个核（核 1）。相机初始化必须在核 0，但它不在冲洗期间进行。
 */
#define LAB_CORE            0
#define LAB_PRIORITY        3       /*!< 低于界面渲染：冲洗时界面照常流畅 */
#define WRITER_PRIORITY     2       /*!< 后台写盘：只用暗房和界面剩下的 CPU */
#define SHARE_CORE          1
#define SHARE_PRIORITY      4
#define POWER_CORE          0
/*
 * 高于同核的相机与暗房：每笔 I2C 传输要等本任务再次运行才释放总线锁，优先级低了会被取景转换
 * 抢占，拖住共享总线上的触摸与 IMU 读取。它 15 s 才醒一次、读完即睡，占不了多少 CPU。
 */
#define POWER_PRIORITY      (CAMERA_PRIORITY + 1)
#define POWER_PERIOD_MS     15000
#define SETTINGS_NAMESPACE  "film"
#define SETTINGS_KEY        "settings"
#define ACCEL_MIN_PERIOD_US (40 * 1000) /*!< 25 Hz：摇一摇峰间隔 ≥110 ms，足够；也减轻共享 I2C 总线负担 */
#define ACCEL_READ_ATTEMPTS 3
#define ACCEL_MIN_VALID_G2  0.04f       /*!< |a| < 0.2 g 视为传感器尚未出数 */
#define TIME_VALID_AFTER    1735689600  /*!< 2025-01-01：早于此说明时钟没校过 */

/*
 * BMI270 读数 → 屏幕坐标里的重力方向（x 向右、y 向下、z 朝外）。
 * 加速度计读的是支撑力（与重力反向），所以朝上的轴读 +1 g；
 * 按传感器 +Y 指向屏幕上沿、+X 指向右、+Z 朝外：重力 = (-ax, +ay, -az)。
 */
static const struct {
    uint8_t source;     /*!< 取传感器的哪个轴（0=x 1=y 2=z） */
    float sign;
} k_axis_map[3] = { { 0, -1.0f }, { 1, 1.0f }, { 2, -1.0f } };

struct film_port_dev_t {
    film_port_t table;
    film_port_dev_config_t config;
    QueueHandle_t events;       /*!< 相机、暗房、分享 → 界面 */
    film_camera_handle_t camera;
    film_lab_handle_t lab;
    film_power_handle_t power;  /*!< NULL 表示读不到电量（界面隐藏电量指示） */
    const char *root;
    bool imu_ready;
    bool accel_logged;          /*!< 首个有效读数已打日志（用于核对轴向） */
    int64_t accel_at;
    float accel[3];
};

/* ---------------------------------------------------------------- 取景 */

static const film_frame_t *dev_preview_acquire(void *ctx)
{
    film_port_dev_handle_t p = ctx;
    return p->camera ? film_camera_acquire(p->camera) : NULL;
}

static void dev_preview_release(void *ctx, const film_frame_t *frame)
{
    film_port_dev_handle_t p = ctx;
    if (p->camera && frame) {
        film_camera_release(p->camera, frame);
    }
}

static void dev_preview_config(void *ctx, const film_preview_config_t *config)
{
    film_port_dev_handle_t p = ctx;
    if (p->camera) {
        film_camera_set_preview(p->camera, config);
    }
}

static bool dev_camera_ready(void *ctx)
{
    film_port_dev_handle_t p = ctx;
    return p->camera && film_camera_ready(p->camera);
}

static void dev_preview_pause(void *ctx, bool paused)
{
    film_port_dev_handle_t p = ctx;
    if (p->camera) {
        film_camera_pause_preview(p->camera, paused);
    }
}

/* ---------------------------------------------------------------- 拍摄与冲洗 */

/* 相机任务上下文：把快门帧转交暗房 */
static void on_still(void *ctx, const film_shot_t *shot, const film_still_t *still,
                     const film_viewfinder_source_t *proof, esp_err_t err)
{
    film_port_dev_handle_t p = ctx;
    film_lab_deliver_still(p->lab, shot, still, proof, err);
}

/* 暗房任务上下文：冲大图时关掉取景流，两个核和驱动帧缓冲都让给冲洗 */
static void on_lab_busy(void *ctx, bool busy)
{
    film_port_dev_handle_t p = ctx;
    if (p->camera) {
        film_camera_hold_preview(p->camera, busy);
    }
}

static esp_err_t dev_shoot(void *ctx, const film_shot_t *shot)
{
    film_port_dev_handle_t p = ctx;
    if (!p->camera) {
        return FILM_ERR_NO_CAMERA;
    }
    ESP_RETURN_ON_ERROR(film_lab_reserve(p->lab), TAG, "lab not ready");
    const esp_err_t err = film_camera_capture(p->camera, shot);
    if (err != ESP_OK) {
        film_lab_cancel_reserve(p->lab);
    }
    return err;
}

static esp_err_t dev_redevelop(void *ctx, const film_shot_t *shot)
{
    film_port_dev_handle_t p = ctx;
    return film_lab_redevelop(p->lab, shot);
}

static void dev_release_result(void *ctx, const uint16_t *screen)
{
    film_port_dev_handle_t p = ctx;
    film_lab_release_screen(p->lab, screen);
}

static bool dev_poll_event(void *ctx, film_event_t *ret_event)
{
    film_port_dev_handle_t p = ctx;
    return xQueueReceive(p->events, ret_event, 0) == pdTRUE;
}

/* ---------------------------------------------------------------- 存储与设置 */

static const char *dev_storage_root(void *ctx)
{
    return ((film_port_dev_handle_t)ctx)->root;
}

static bool dev_settings_load(void *ctx, void *data, size_t len)
{
    (void)ctx;
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = len;
    const esp_err_t err = nvs_get_blob(nvs, SETTINGS_KEY, data, &size);
    nvs_close(nvs);
    return err == ESP_OK && size == len;
}

static void dev_settings_save(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    nvs_handle_t nvs;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_set_blob(nvs, SETTINGS_KEY, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings save: %s", esp_err_to_name(err));
    }
}

/* ---------------------------------------------------------------- 分享 */

static esp_err_t dev_share_start(void *ctx, const uint32_t *ids, size_t count, bool force_hotspot)
{
    (void)ctx;
    return film_share_begin(ids, count, force_hotspot);
}

static void dev_share_stop(void *ctx)
{
    (void)ctx;
    film_share_end();
}

static void dev_share_status(void *ctx, film_share_status_t *ret_status)
{
    (void)ctx;
    film_share_get_status(ret_status);
}

/* ---------------------------------------------------------------- 反馈与传感器 */

static void dev_feedback(void *ctx, film_feedback_t kind)
{
    film_port_dev_handle_t p = ctx;
    film_feedback_play(p->config.feedback, kind);
}

static bool dev_read_accel(void *ctx, float *x, float *y, float *z)
{
    film_port_dev_handle_t p = ctx;
    if (!p->imu_ready) {
        return false;
    }
    const int64_t now = esp_timer_get_time();
    if (now - p->accel_at >= ACCEL_MIN_PERIOD_US) {
        float raw[3];
        /*
         * 主板 I2C 由触摸、IMU、模块 EEPROM 探测和相机 SCCB 共用。当前 IDF 的 i2c_master
         * 在“含读操作的传输被 NACK”后不清 contains_read，下一笔纯写传输（如模块探测）
         * 的中断会把 FIFO 写进已失效的缓冲区而崩溃。失败后立刻再读一次：成功的读会清掉该状态。
         */
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < ACCEL_READ_ATTEMPTS && err != ESP_OK; ++attempt) {
            err = bsp_imu_get_accel(&raw[0], &raw[1], &raw[2]);
        }
        if (err != ESP_OK) {
            p->accel_at = now;   /* 总线异常时也按周期限速，不要连续猛读 */
            return false;
        }
        /* 刚启动时 BMI270 还没出第一组数据，全零读数不能当成“失重” */
        const float g2 = raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2];
        if (g2 < ACCEL_MIN_VALID_G2) {
            return false;
        }
        if (!p->accel_logged) {
            p->accel_logged = true;
            ESP_LOGI(TAG, "IMU first sample, raw accel %+.2f %+.2f %+.2f g", raw[0], raw[1], raw[2]);
        }
        for (int i = 0; i < 3; ++i) {
            p->accel[i] = k_axis_map[i].sign * raw[k_axis_map[i].source];
        }
        p->accel_at = now;
    }
    *x = p->accel[0];
    *y = p->accel[1];
    *z = p->accel[2];
    return true;
}

static bool dev_wall_time(void *ctx, int64_t *ret_time)
{
    (void)ctx;
    const time_t now = time(NULL);
    if (!film_share_time_synced() && now < TIME_VALID_AFTER) {
        return false;
    }
    *ret_time = (int64_t)now;
    return true;
}

static bool dev_read_battery(void *ctx, film_battery_t *ret_battery)
{
    film_port_dev_handle_t p = ctx;
    return film_power_get(p->power, ret_battery);
}

/* ---------------------------------------------------------------- 创建 */

static void start_imu(film_port_dev_handle_t p)
{
    const bsp_imu_config_t config = BSP_IMU_CONFIG_DEFAULT();
    p->imu_ready = bsp_imu_init() == ESP_OK && bsp_imu_start(&config) == ESP_OK;
    if (p->imu_ready) {
        ESP_LOGI(TAG, "IMU ready");
    } else {
        ESP_LOGW(TAG, "IMU unavailable: orientation and shake disabled");
    }
}

esp_err_t film_port_dev_create(const film_port_dev_config_t *config, film_port_dev_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "bad args");
    film_port_dev_handle_t p = calloc(1, sizeof(*p));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "port");
    p->config = *config;
    p->events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(film_event_t));
    if (!p->events) {
        free(p);
        return ESP_ERR_NO_MEM;
    }

    /* 存储失败不致命：只是不能存照片 */
    if (film_storage_mount() == ESP_OK) {
        p->root = film_storage_photo_dir();
    }
    const film_lab_config_t lab = {
        .core = LAB_CORE, .priority = LAB_PRIORITY, .writer_priority = WRITER_PRIORITY, .events = p->events,
        .on_busy = on_lab_busy, .ctx = p,
    };
    esp_err_t err = film_lab_start(&lab, &p->lab);
    if (err != ESP_OK) {
        vQueueDelete(p->events);
        free(p);
        return err;
    }
    film_lab_set_root(p->lab, p->root);

    const film_camera_config_t camera = {
        .core = CAMERA_CORE, .priority = CAMERA_PRIORITY, .on_still = on_still, .ctx = p,
    };
    if (film_camera_start(&camera, &p->camera) != ESP_OK) {
        p->camera = NULL;   /* 界面显示"相机未连接" */
    }

    const film_share_config_t share = { .events = p->events, .core = SHARE_CORE, .priority = SHARE_PRIORITY };
    if (film_share_init(&share) == ESP_OK) {
        film_share_set_root(p->root);
    } else {
        ESP_LOGW(TAG, "sharing unavailable");
    }
    start_imu(p);
    const film_power_config_t power = { .core = POWER_CORE, .priority = POWER_PRIORITY, .period_ms = POWER_PERIOD_MS };
    if (film_power_start(&power, &p->power) != ESP_OK) {
        p->power = NULL;
        ESP_LOGW(TAG, "battery monitor unavailable");
    }

    p->table = (film_port_t){
        .ctx = p,
        .preview_acquire = dev_preview_acquire,
        .preview_release = dev_preview_release,
        .preview_config = dev_preview_config,
        .camera_ready = dev_camera_ready,
        .preview_pause = dev_preview_pause,
        .shoot = dev_shoot,
        .redevelop = dev_redevelop,
        .release_result = dev_release_result,
        .poll_event = dev_poll_event,
        .storage_root = dev_storage_root,
        .settings_load = dev_settings_load,
        .settings_save = dev_settings_save,
        .share_start = dev_share_start,
        .share_stop = dev_share_stop,
        .share_status = dev_share_status,
        .feedback = dev_feedback,
        .read_accel = dev_read_accel,
        .wall_time = dev_wall_time,
        .read_battery = dev_read_battery,
    };
    *ret_handle = p;
    return ESP_OK;
}

const film_port_t *film_port_dev_table(film_port_dev_handle_t handle)
{
    return &handle->table;
}

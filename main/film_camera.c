#include "film_camera.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "film_still.h"
#include "film_viewfinder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaico_module_camera.h"
#include "mosaico_module_mgr.h"

static const char *TAG = "film_camera";

#define CAMERA_TASK_STACK        6144
#define SENSOR_WIDTH             640
#define SENSOR_HEIGHT            480
#define FRAME_TIMEOUT_MS         1000
#define RETRY_DELAY_MS           2000     /*!< 相机缺失时多久重新探测一次（支持热插拔） */
#define RETRY_DELAY_MAX_MS       8000     /*!< 连续失败时退避的上限，避免反复初始化拖垮 SCCB 总线 */
#define MAX_FRAME_ERRORS         5        /*!< 连续取帧失败这么多次视为相机被拔掉 */
#define MODULE_SCAN_PERIOD_MS    (60 * 60 * 1000)   /*!< 实际上关闭周期探测，见 start_module_manager */
#define HOLD_POLL_MS             30
#define HOLD_WAIT_MS             1500     /*!< 暂停取景时最多等相机任务关流多久（一帧 + 关流） */
#define STATS_INTERVAL_US        (10 * 1000 * 1000)
#define SLOT_COUNT               3        /*!< 一张界面在用、一张待取、一张正在写 */
#define SLOT_PIXELS              (FILM_VF_M6_W * FILM_VF_M6_H)
#define BUFFER_CAPS              (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BUFFER_ALIGN             64

typedef struct {
    film_frame_t frame;
    uint16_t *pixels;
} slot_t;

struct film_camera_t {
    film_camera_config_t config;
    TaskHandle_t task;

    /* lock 保护下面这一组：界面任务与相机任务共享 */
    SemaphoreHandle_t lock;
    film_preview_config_t preview;
    bool preview_wanted;        /*!< 界面设置过取景参数（开机动画结束后才会） */
    bool ready;
    bool hold;
    bool paused;                /*!< 界面暂时不要新帧：取帧后直接归还，不转换 */
    bool capture_pending;
    bool capturing;
    film_shot_t pending;
    int8_t ready_slot;          /*!< 待取的最新帧，-1 表示没有 */
    uint8_t held_mask;          /*!< 界面正在用的帧 */
    uint32_t seq;
    slot_t slots[SLOT_COUNT];

    SemaphoreHandle_t stopped;  /*!< 相机任务在暂停状态下关掉取景流后给出 */

    /* 以下只在相机任务里使用 */
    mosaico_camera_handle_t camera;
    bool streaming;
    uint32_t stat_frames;
    int64_t stat_since;
    int64_t stat_convert_us;
};

/* ---------------------------------------------------------------- 取景缓冲 */

static int pick_write_slot(film_camera_handle_t cam)
{
    int slot = -1;
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (i != cam->ready_slot && !(cam->held_mask & (1u << i))) {
            slot = i;
            break;
        }
    }
    xSemaphoreGive(cam->lock);
    return slot;
}

static void publish_slot(film_camera_handle_t cam, int slot, uint16_t w, uint16_t h)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->slots[slot].frame.width = w;
    cam->slots[slot].frame.height = h;
    cam->slots[slot].frame.seq = ++cam->seq;
    cam->ready_slot = (int8_t)slot;
    xSemaphoreGive(cam->lock);
}

const film_frame_t *film_camera_acquire(film_camera_handle_t cam)
{
    const film_frame_t *frame = NULL;
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    if (cam->ready_slot >= 0) {
        frame = &cam->slots[cam->ready_slot].frame;
        cam->held_mask |= (uint8_t)(1u << cam->ready_slot);
        cam->ready_slot = -1;
    }
    xSemaphoreGive(cam->lock);
    return frame;
}

void film_camera_release(film_camera_handle_t cam, const film_frame_t *frame)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    for (int i = 0; i < SLOT_COUNT; ++i) {
        if (frame == &cam->slots[i].frame) {
            cam->held_mask &= (uint8_t)~(1u << i);
        }
    }
    xSemaphoreGive(cam->lock);
}

/* ---------------------------------------------------------------- 公共请求 */

bool film_camera_ready(film_camera_handle_t cam)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    const bool ready = cam->ready;
    xSemaphoreGive(cam->lock);
    return ready;
}

void film_camera_set_preview(film_camera_handle_t cam, const film_preview_config_t *config)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->preview = *config;
    cam->preview_wanted = true;
    xSemaphoreGive(cam->lock);
}

void film_camera_hold_preview(film_camera_handle_t cam, bool hold)
{
    (void)xSemaphoreTake(cam->stopped, 0);   /* 丢掉上一次暂停留下的信号 */
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->hold = hold;
    const bool wait = hold && cam->ready;   /* 没有相机就没有流要关 */
    xSemaphoreGive(cam->lock);
    xTaskNotifyGive(cam->task);
    if (wait && xSemaphoreTake(cam->stopped, pdMS_TO_TICKS(HOLD_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "preview did not stop within %d ms", HOLD_WAIT_MS);
    }
}

void film_camera_pause_preview(film_camera_handle_t cam, bool paused)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->paused = paused;
    xSemaphoreGive(cam->lock);
}

esp_err_t film_camera_capture(film_camera_handle_t cam, const film_shot_t *shot)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    if (!cam->ready) {
        err = FILM_ERR_NO_CAMERA;
    } else if (cam->capture_pending || cam->capturing) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        cam->pending = *shot;
        cam->capture_pending = true;
        cam->hold = true;   /* 拍完后取景保持暂停，等暗房冲洗完再恢复 */
    }
    xSemaphoreGive(cam->lock);
    if (err == ESP_OK) {
        xTaskNotifyGive(cam->task);   /* 叫醒可能在等待的相机任务 */
    }
    return err;
}

/* ---------------------------------------------------------------- 相机生命周期（相机任务） */

static void set_ready(film_camera_handle_t cam, bool ready)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->ready = ready;
    xSemaphoreGive(cam->lock);
}

static esp_err_t stream_start(film_camera_handle_t cam)
{
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_ERROR(mosaico_camera_open(cam->camera), TAG, "open");
    esp_err_t err = mosaico_camera_start_stream(cam->camera);
    if (err != ESP_OK) {
        (void)mosaico_camera_close(cam->camera);
        return err;
    }
    cam->streaming = true;
    mosaico_camera_info_t info = { 0 };
    (void)mosaico_camera_get_info(cam->camera, &info);
    ESP_LOGI(TAG, "stream PSRAM %u KB (%u buffers x %u KB)",
             (unsigned)((free_before - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / 1024),
             (unsigned)info.buffer_count, (unsigned)(info.frame_buffer_size / 1024));
    return ESP_OK;
}

static void stream_stop(film_camera_handle_t cam)
{
    if (cam->streaming) {
        (void)mosaico_camera_stop_stream(cam->camera);
        (void)mosaico_camera_close(cam->camera);
        cam->streaming = false;
    }
}

static esp_err_t camera_attach(film_camera_handle_t cam)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.width = SENSOR_WIDTH;
    config.height = SENSOR_HEIGHT;
    config.frame_timeout_ms = FRAME_TIMEOUT_MS;
    ESP_RETURN_ON_ERROR(mosaico_camera_new(&config, &cam->camera), TAG, "camera new");
    const esp_err_t err = stream_start(cam);
    if (err != ESP_OK) {
        (void)mosaico_camera_del(cam->camera);
        cam->camera = NULL;
        return err;
    }
    mosaico_camera_info_t info = { 0 };
    (void)mosaico_camera_get_info(cam->camera, &info);
    ESP_LOGI(TAG, "camera ready: %lux%lu @%lu fps", (unsigned long)info.width, (unsigned long)info.height,
             (unsigned long)info.frame_rate);
    set_ready(cam, true);
    return ESP_OK;
}

static void camera_detach(film_camera_handle_t cam)
{
    set_ready(cam, false);
    stream_stop(cam);
    (void)mosaico_camera_del(cam->camera);
    cam->camera = NULL;
    ESP_LOGW(TAG, "camera lost");
}

/* ---------------------------------------------------------------- 取景一帧 */

static void log_stats(film_camera_handle_t cam)
{
    const int64_t now = esp_timer_get_time();
    if (cam->stat_since == 0) {
        cam->stat_since = now;
    } else if (now - cam->stat_since >= STATS_INTERVAL_US) {
        const float seconds = (float)(now - cam->stat_since) / 1e6f;
        ESP_LOGI(TAG, "preview %.1f fps, convert %.1f ms, PSRAM free %u KB", cam->stat_frames / seconds,
                 cam->stat_frames ? (float)cam->stat_convert_us / 1000.0f / (float)cam->stat_frames : 0.0f,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        cam->stat_frames = 0;
        cam->stat_convert_us = 0;
        cam->stat_since = now;
    }
}

/*
 * 取一帧，裁切转向后写进空闲槽（不加胶卷效果，胶卷只在冲洗成片时生效）。
 * 只用 CPU：显示刷新要用 2D-DMA，取景再用 PPA 抢 2D-DMA 时整个队列曾被卡死。
 * 返回取帧本身的错误（用于判断相机是否掉线）。
 * convert 为 false 时（界面暂停取景）只取回再归还：流不断，取帧照样按帧率限速，掉线照样能发现。
 */
static esp_err_t preview_step(film_camera_handle_t cam, const film_preview_config_t *pv, bool convert)
{
    mosaico_camera_frame_t frame = { 0 };
    ESP_RETURN_ON_ERROR(mosaico_camera_get_frame(cam->camera, &frame), TAG, "get frame");
    const int slot = convert ? pick_write_slot(cam) : -1;
    if (slot >= 0) {
        const int64_t t0 = esp_timer_get_time();
        const film_viewfinder_source_t src = {
            .uyvy = frame.data, .width = frame.width, .height = frame.height, .stride = frame.bytes_per_line,
        };
        const esp_err_t err = film_viewfinder_convert(&src, pv->instant, cam->slots[slot].pixels);
        if (err == ESP_OK) {
            uint16_t vw, vh;
            film_viewfinder_size(pv->instant, &vw, &vh);
            publish_slot(cam, slot, vw, vh);
            cam->stat_frames++;
            cam->stat_convert_us += esp_timer_get_time() - t0;
        } else {
            ESP_LOGW(TAG, "preview frame %lux%lu: %s", (unsigned long)frame.width, (unsigned long)frame.height,
                     esp_err_to_name(err));
        }
    }
    (void)mosaico_camera_return_frame(cam->camera, &frame);
    log_stats(cam);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 拍照 */

/**
 * 复制下一帧取景原始数据（约 600 KB，最多等一帧）。暗房拿它在几百毫秒内冲出屏幕尺寸的小样，
 * 不必等 2048×1536 原片几秒钟的软件解码。拿不到就返回空来源，不影响拍照。
 */
static film_viewfinder_source_t grab_proof(film_camera_handle_t cam)
{
    film_viewfinder_source_t proof = { 0 };
    mosaico_camera_frame_t frame = { 0 };
    if (!cam->streaming || mosaico_camera_get_frame(cam->camera, &frame) != ESP_OK) {
        return proof;
    }
    const size_t stride = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2u;
    const size_t bytes = stride * frame.height;
    uint8_t *copy = bytes <= frame.size ? heap_caps_malloc(bytes, BUFFER_CAPS) : NULL;
    if (copy) {
        memcpy(copy, frame.data, bytes);
        proof = (film_viewfinder_source_t){
            .uyvy = copy, .width = frame.width, .height = frame.height, .stride = stride,
        };
    }
    (void)mosaico_camera_return_frame(cam->camera, &frame);
    return proof;
}

static void capture(film_camera_handle_t cam, const film_shot_t *shot)
{
    film_still_t still;
    const film_viewfinder_source_t proof = grab_proof(cam);
    /* 取景流在暂停期间一直关着（驱动帧缓冲让给暗房），由主循环在恢复时重开 */
    stream_stop(cam);
    esp_err_t err = film_still_capture(&still);
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->capturing = false;
    xSemaphoreGive(cam->lock);
    cam->config.on_still(cam->config.ctx, shot, &still, &proof, err);
}

static void camera_task(void *arg)
{
    film_camera_handle_t cam = arg;
    int frame_errors = 0;
    bool logged_missing = false;
    uint32_t retry_ms = RETRY_DELAY_MS;
    for (;;) {
        if (!cam->camera) {
            const esp_err_t err = camera_attach(cam);
            if (err != ESP_OK) {
                if (!logged_missing) {
                    ESP_LOGW(TAG, "camera unavailable (%s), retrying", esp_err_to_name(err));
                    logged_missing = true;
                }
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(retry_ms));
                retry_ms = retry_ms * 2u > RETRY_DELAY_MAX_MS ? RETRY_DELAY_MAX_MS : retry_ms * 2u;
                /* 周期探测已关闭：重试前显式重扫一次，热插拔才能被发现 */
                (void)mosaico_module_mgr_request_rescan(MOSAICO_MODULE_MGR_SLOT_LEFT);
                continue;
            }
            logged_missing = false;
            retry_ms = RETRY_DELAY_MS;
            frame_errors = 0;
        }
        xSemaphoreTake(cam->lock, portMAX_DELAY);
        const film_preview_config_t pv = cam->preview;
        const bool hold = cam->hold;
        const bool paused = cam->paused;
        const bool wanted = cam->preview_wanted;
        const bool shoot = cam->capture_pending;
        const film_shot_t shot = cam->pending;
        if (shoot) {
            cam->capture_pending = false;
            cam->capturing = true;
        }
        xSemaphoreGive(cam->lock);

        if (shoot) {
            capture(cam, &shot);
            continue;
        }
        /*
         * 暂停（暗房冲洗）或界面还没要取景（开机动画期间）：关掉取景流。
         * 关流释放驱动帧缓冲给暗房；开机动画期间把 CPU 留给动画解码与渲染。
         */
        if (hold || !wanted) {
            stream_stop(cam);
            if (hold) {
                xSemaphoreGive(cam->stopped);
            }
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HOLD_POLL_MS));
            continue;
        }
        if (!cam->streaming && stream_start(cam) != ESP_OK) {
            ESP_LOGW(TAG, "resume preview failed");
            camera_detach(cam);
            continue;
        }
        if (preview_step(cam, &pv, !paused) != ESP_OK) {
            if (++frame_errors >= MAX_FRAME_ERRORS) {
                camera_detach(cam);
            }
        } else {
            frame_errors = 0;
        }
    }
}

/* ---------------------------------------------------------------- 创建 */

static void free_buffers(film_camera_handle_t cam)
{
    for (int i = 0; i < SLOT_COUNT; ++i) {
        heap_caps_free(cam->slots[i].pixels);
    }
    if (cam->stopped) {
        vSemaphoreDelete(cam->stopped);
    }
    if (cam->lock) {
        vSemaphoreDelete(cam->lock);
    }
    free(cam);
}

/*
 * 模块管理器默认每 250 ms 用 i2c_master_probe 探测 EEPROM。v1.0 主板上这条 I2C 与触摸、
 * IMU、相机 SCCB 共用，而当前 IDF 的 probe 在任何一次读传输被 NACK 之后会在中断里写坏内存。
 * 所以由本应用先启动管理器、关掉周期探测，只在需要时（开机、找相机、释放相机）显式重扫；
 * 相机拔出改由连续取帧失败判断。mosaico_camera_new 内部的 init(NULL) 会复用这里的配置。
 */
static esp_err_t start_module_manager(void)
{
    mosaico_module_mgr_config_t config = MOSAICO_MODULE_MGR_DEFAULT_CONFIG();
    config.scan_period_ms = MODULE_SCAN_PERIOD_MS;
    config.debounce_count = 1;   /* 只剩显式重扫，一次结果就要生效 */
    return mosaico_module_mgr_init(&config);
}

esp_err_t film_camera_start(const film_camera_config_t *config, film_camera_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && config->on_still && ret_handle, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_ERROR(start_module_manager(), TAG, "module manager");
    film_camera_handle_t cam = calloc(1, sizeof(*cam));
    ESP_RETURN_ON_FALSE(cam, ESP_ERR_NO_MEM, TAG, "camera");
    cam->config = *config;
    cam->ready_slot = -1;
    cam->preview = (film_preview_config_t){ .film = FILM_ID_GOLD };
    cam->lock = xSemaphoreCreateMutex();
    cam->stopped = xSemaphoreCreateBinary();
    bool ok = cam->lock && cam->stopped;
    for (int i = 0; ok && i < SLOT_COUNT; ++i) {
        cam->slots[i].pixels = heap_caps_aligned_calloc(BUFFER_ALIGN, 1, SLOT_PIXELS * 2u, BUFFER_CAPS);
        cam->slots[i].frame.pixels = cam->slots[i].pixels;
        ok = cam->slots[i].pixels != NULL;
    }
    esp_err_t err = ok ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = xTaskCreatePinnedToCore(camera_task, "film_cam", CAMERA_TASK_STACK, cam, config->priority,
                                      &cam->task, config->core) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        free_buffers(cam);
        return err;
    }
    *ret_handle = cam;
    return ESP_OK;
}

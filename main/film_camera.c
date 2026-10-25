#include "film_camera.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "film_capture_mode.h"
#include "film_filter.h"
#include "film_perf.h"
#include "film_sensor.h"
#include "film_still.h"
#include "film_viewfinder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mosaico_module_camera.h"
#include "mosaico_module_mgr.h"

static const char *TAG = "film_camera";

#define CAMERA_TASK_STACK        10240   /*!< 拍照路径（取帧校验、传感器切换、on_still 回调）在这个任务里跑，6 KB 余量不明 */
#define FILM_CAMERA_STATUS_LEN   48
#define FRAME_TIMEOUT_MS         1000
#define PREVIEW_DEFAULT_BUFFERS  2        /*!< 创建 BSP 相机时的取景缓冲个数；film_capture_mode 选了别的个数就重建一次 */
#define RETRY_DELAY_MS           2000     /*!< 相机缺失时多久重新探测一次（支持热插拔） */
#define RETRY_DELAY_MAX_MS       8000     /*!< 连续失败时退避的上限，避免反复初始化拖垮 SCCB 总线 */
#define MAX_FRAME_ERRORS         5        /*!< 连续取帧失败这么多次视为相机被拔掉 */
#define MODULE_SCAN_PERIOD_MS    (60 * 60 * 1000)   /*!< 实际上关闭周期探测，见 start_module_manager */
#define HOLD_POLL_MS             30
#define HOLD_WAIT_MS             1500     /*!< 暂停取景时最多等相机任务关流多久（一帧 + 关流） */
/*
 * 界面暂停取景超过这么久就关流（传感器与 CSI 停下）。短暂停顿（打开机身面板、进相册看一眼
 * 就回来）不关，免得回到取景时等重新开流。
 */
#define PAUSE_STREAM_OFF_US      (3 * 1000 * 1000)
#define PAUSED_POLL_MS           1000     /*!< 关流暂停期间的兜底唤醒（恢复时会被立即叫醒） */
/*
 * 流开着却这么久没取到一帧，界面就提示相机没有出图（模块接触不良时 SCCB 正常、一帧不出）。
 * 开流到第一帧不到 1 s；连续 MAX_FRAME_ERRORS 次超时后才会断开重连，这期间别让界面停在旧帧上。
 */
#define PREVIEW_STALL_US         (2 * 1000 * 1000)
#define STATS_INTERVAL_US        (10 * 1000 * 1000)
/*
 * 自动闪光（film_still_exposure_index，16 = 一帧满曝光、1 倍增益）：曝光已经拉满一帧、增益到 4 倍还不够才点灯。
 * 初值，按真机暗光实拍再调；每次拍照的日志里都有这次的读数。
 */
#define FLASH_FIRE_INDEX         (4u * FILM_STILL_EXPOSURE_UNIT)
/*
 * 没有曝光寄存器可读的传感器（SC101IOT）改看取景帧 4:3 窗口的平均亮度（Y，16–235）：
 * 自动曝光已经补不上来、画面仍然这么暗才点灯。初值，按真机暗光实拍再调。
 */
#define FLASH_FIRE_LUMA          40u
#define LUMA_SAMPLE_STEP         16       /*!< 测亮度时横竖每隔这么多像素取一个（偶数：落在 UYVY 的 Y0 上） */
/* 点灯后等取景的自动曝光在灯光下收敛：至少这么多帧，前后两帧读数相差不到 1/8 就算稳了，最多等这么久 */
#define FLASH_SETTLE_MIN_FRAMES  3
#define FLASH_SETTLE_MAX_US      (600 * 1000)
#define SLOT_COUNT               3        /*!< 一张界面在用、一张待取或正在写，一张给界面换帧的瞬间 */
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
    char status[FILM_CAMERA_STATUS_LEN];   /*!< film_camera_describe 的内容，相机任务在连接成败时更新 */
    bool hold;
    bool paused;                /*!< 界面暂时不要新帧：取帧后直接归还，不转换 */
    bool capture_pending;
    bool capturing;
    int64_t frame_wait_since;   /*!< 开流或上一次取到帧的时刻；0 表示没在取景（流关着） */
    film_shot_t pending;
    int8_t ready_slot;          /*!< 待取的最新帧，-1 表示没有 */
    uint8_t held_mask;          /*!< 界面正在用的帧 */
    uint32_t seq;
    slot_t slots[SLOT_COUNT];

    SemaphoreHandle_t stopped;  /*!< 相机任务在暂停状态下关掉取景流后给出 */

    /* 以下只在相机任务里使用 */
    mosaico_camera_handle_t camera;
    film_capture_mode_t modes[FILM_CAPTURE_MODE_MAX];   /*!< 连接时枚举到的传感器格式（film_capture_mode 的输入） */
    bool jpeg_still;            /*!< 拍照走 film_still 的 JPEG；否则快门帧就是取景同款的 UYVY 帧 */
    bool exposure_meter;        /*!< 能读曝光寄存器（OV3640）：自动闪光看曝光读数，否则看取景帧亮度 */
    uint16_t still_width;       /*!< jpeg_still 时的 JPEG 尺寸 */
    uint16_t still_height;
    bool streaming;
    int64_t paused_since;       /*!< 界面开始暂停取景的时刻，0 表示没暂停 */
    uint32_t stat_frames;
    int64_t stat_since;
    int64_t stat_convert_us;
    /* 胶卷取景外观：转换表（内部 RAM，分配失败则取景保持原样）与它对应的胶卷/曝光 */
    film_viewfinder_look_t *look;
    int look_film;
    int look_ev_centi;
#ifdef CONFIG_FILM_PERF_TRACE
    /* 取景每帧分段耗时（perf vf）：采集等待、界面取走延迟、因界面没取走而丢掉的帧 */
    int64_t perf_wait_us;           /*!< mosaico_camera_get_frame 等帧的累计时间 */
    uint32_t perf_wait_frames;
    int64_t perf_publish_us[SLOT_COUNT];    /*!< 各槽最近一次发布的时刻（相机任务写，界面任务读，lock 保护） */
    int64_t perf_latency_us;        /*!< 发布 → 界面取走 的累计时间 */
    uint32_t perf_acquired;
    uint32_t perf_dropped;          /*!< 取到帧但没有空槽（界面没跟上），直接丢弃的帧数 */
#endif
};

/* ---------------------------------------------------------------- 取景缓冲 */

/**
 * 找一个能写的槽。上一帧界面还没取走时返回 -1：界面重绘比相机慢，多转的帧没人看，
 * 只白占 CPU。界面一取走就转下一帧，赶在下次重绘前出好。
 */
static int pick_write_slot(film_camera_handle_t cam)
{
    int slot = -1;
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    for (int i = 0; i < SLOT_COUNT && cam->ready_slot < 0; ++i) {
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
#ifdef CONFIG_FILM_PERF_TRACE
    cam->perf_publish_us[slot] = esp_timer_get_time();
#endif
    xSemaphoreGive(cam->lock);
    film_perf_preview_frame();
}

const film_frame_t *film_camera_acquire(film_camera_handle_t cam)
{
    const film_frame_t *frame = NULL;
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    if (cam->ready_slot >= 0) {
        frame = &cam->slots[cam->ready_slot].frame;
#ifdef CONFIG_FILM_PERF_TRACE
        cam->perf_latency_us += esp_timer_get_time() - cam->perf_publish_us[cam->ready_slot];
        ++cam->perf_acquired;
#endif
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

/** 暂停或拍照结束、重新开始要帧时从现在起计时（持有 lock 调用） */
static void restart_stall_clock(film_camera_handle_t cam)
{
    if (cam->frame_wait_since != 0) {
        cam->frame_wait_since = esp_timer_get_time();
    }
}

bool film_camera_preview_stalled(film_camera_handle_t cam)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    /* 拍照、暂停取景期间本来就不出新帧，不算 */
    const bool expecting = cam->ready && !cam->hold && !cam->paused && !cam->capturing && !cam->capture_pending &&
                           cam->frame_wait_since != 0;
    const int64_t since = cam->frame_wait_since;
    xSemaphoreGive(cam->lock);
    return expecting && esp_timer_get_time() - since >= PREVIEW_STALL_US;
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
    if (!hold) {
        restart_stall_clock(cam);
    }
    xSemaphoreGive(cam->lock);
    xTaskNotifyGive(cam->task);
    if (wait && xSemaphoreTake(cam->stopped, pdMS_TO_TICKS(HOLD_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "preview did not stop within %d ms", HOLD_WAIT_MS);
    }
}

void film_camera_pause_preview(film_camera_handle_t cam, bool paused)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    const bool changed = cam->paused != paused;
    cam->paused = paused;
    if (changed && !paused) {
        restart_stall_clock(cam);
    }
    xSemaphoreGive(cam->lock);
    if (changed && !paused) {
        xTaskNotifyGive(cam->task);   /* 流可能已经关了：叫醒相机任务立刻重开 */
    }
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
        /*
         * 拍—冲串行：拍完立刻接着冲洗，用不上取景。受理快门时就把取景置为暂停，
         * 拍完流不再重开（省一次关/开流），由暗房冲洗结束（或拍照失败）时 hold_preview(false) 恢复。
         * 本次拍摄仍先用着现有的取景流取小样（相机任务先处理拍摄再看 hold）。
         */
        cam->hold = true;
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

/** 记下"从现在起等下一帧"：开流时与每取到一帧时调用；when 为 0 表示流已关、不再等帧 */
static void set_frame_wait(film_camera_handle_t cam, int64_t when)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->frame_wait_since = when;
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
    set_frame_wait(cam, esp_timer_get_time());
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
        set_frame_wait(cam, 0);
    }
}

static void set_status(film_camera_handle_t cam, const char *text)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    snprintf(cam->status, sizeof(cam->status), "%s", text);
    xSemaphoreGive(cam->lock);
}

void film_camera_describe(film_camera_handle_t cam, char *buf, size_t size)
{
    if (!buf || !size) {
        return;
    }
    if (!cam) {
        snprintf(buf, size, "UNAVAILABLE");
        return;
    }
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    snprintf(buf, size, "%s", cam->status[0] ? cam->status : "STARTING");
    xSemaphoreGive(cam->lock);
}

/**
 * 相机没连上时判断原因，写进日志和状态（"相机未连接"的用户反馈靠它定位）：
 *   模块没响应 EEPROM / EEPROM 读到了但描述符无效 / 描述符有效但不是相机板 / 其余（格式、传感器、取流）。
 * 重试期间原因没变时不重复刷日志。
 */
static void diagnose_failure(film_camera_handle_t cam, esp_err_t err)
{
    mosaico_module_mgr_info_t info = { 0 };
    const char *reason;
    char text[FILM_CAMERA_STATUS_LEN];
    if (mosaico_module_mgr_get_info(MOSAICO_MODULE_MGR_SLOT_LEFT, &info) != ESP_OK) {
        reason = "module manager unavailable";
        snprintf(text, sizeof(text), "ERR %s", esp_err_to_name(err));
    } else if (info.presence != MOSAICO_MODULE_PRESENCE_PRESENT) {
        reason = "no module acknowledged on the left slot";
        snprintf(text, sizeof(text), "NO MODULE");
    } else if (info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_INVALID) {
        reason = "module present but its EEPROM descriptor is invalid";
        snprintf(text, sizeof(text), "BAD EEPROM");
    } else if (info.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_VALID &&
               info.eeprom.board_type != (uint8_t)MOSAICO_BOARD_TYPE_CAMERA) {
        reason = "module present but not a camera board";
        snprintf(text, sizeof(text), "NOT A CAMERA");
    } else {
        reason = "camera board found but the sensor did not start";
        snprintf(text, sizeof(text), "ERR %s", esp_err_to_name(err));
    }
    char previous[FILM_CAMERA_STATUS_LEN];
    film_camera_describe(cam, previous, sizeof(previous));
    if (strcmp(previous, text) != 0) {
        ESP_LOGW(TAG, "camera unavailable: %s (%s; presence %d, descriptor %d, module error %s)", reason,
                 esp_err_to_name(err), (int)info.presence, (int)info.descriptor_state,
                 esp_err_to_name(info.last_error));
    }
    set_status(cam, text);
}

/** 注册 BSP 相机：0×0 表示沿用传感器当前格式（Kconfig 默认，或 film_sensor 切好的格式） */
static esp_err_t camera_new(film_camera_handle_t cam, bool allow_unidentified, uint8_t buffers)
{
    mosaico_camera_config_t config = MOSAICO_CAMERA_DEFAULT_CONFIG();
    config.width = 0;
    config.height = 0;
    config.buffer_count = buffers;
    config.frame_timeout_ms = FRAME_TIMEOUT_MS;
    config.allow_unidentified = allow_unidentified;
    return mosaico_camera_new(&config, &cam->camera);
}

/** 枚举传感器格式，按此刻（开流之前）的 PSRAM 余量选取景格式、缓冲个数与拍照来源 */
static void plan_capture(film_camera_handle_t cam, uint16_t pid, film_capture_plan_t *plan)
{
    size_t count = 0;
    if (film_sensor_list_modes(cam->modes, FILM_CAPTURE_MODE_MAX, &count) != ESP_OK) {
        count = 0;
    }
    const film_capture_env_t env = {
        .jpeg_still = film_still_sensor_has_jpeg(pid),
        .psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        .psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
    };
    film_capture_mode_plan(cam->modes, count, &env, plan);
    ESP_LOGI(TAG, "capture plan: %u modes, PSRAM free %u KB (largest %u KB) -> preview mode %d x%u buffers, "
             "still %s %ux%u", (unsigned)count, (unsigned)(env.psram_free / 1024), (unsigned)(env.psram_largest / 1024),
             plan->preview, (unsigned)plan->buffers, plan->still >= 0 ? "JPEG" : "frame", (unsigned)plan->photo_width,
             (unsigned)plan->photo_height);
}

/**
 * 注册并开流一次；任何一步失败都会撤销，返回失败原因。
 * use_plan 为 false 时不切格式、用默认缓冲个数（选定格式开不起来时的退路）；拍照来源照样按传感器能力定。
 */
static esp_err_t attach_once(film_camera_handle_t cam, bool allow_unidentified, bool use_plan)
{
    ESP_RETURN_ON_ERROR(camera_new(cam, allow_unidentified, PREVIEW_DEFAULT_BUFFERS), TAG, "camera new");
    /* 视频设备已注册、还没开流：此时读传感器型号与格式表，决定取景格式和拍照走 JPEG 还是 YUV 帧 */
    uint16_t pid = 0;
    if (film_still_sensor_pid(&pid) != ESP_OK) {
        pid = 0;
    }
    film_capture_plan_t plan;
    plan_capture(cam, pid, &plan);
    if (!use_plan) {
        plan.preview = -1;
        plan.buffers = PREVIEW_DEFAULT_BUFFERS;
    }
    esp_err_t err = ESP_OK;
    if (plan.buffers != PREVIEW_DEFAULT_BUFFERS) {
        /* BSP 相机只在创建时接受缓冲个数：重建一次（传感器回到 Kconfig 默认格式，下面再切） */
        (void)mosaico_camera_del(cam->camera);
        cam->camera = NULL;
        err = camera_new(cam, allow_unidentified, plan.buffers);
    }
    if (err == ESP_OK && plan.preview >= 0) {
        err = film_sensor_apply_mode((size_t)plan.preview);
    }
    if (err == ESP_OK) {
        err = stream_start(cam);
    }
    if (err != ESP_OK) {
        if (cam->camera) {
            (void)mosaico_camera_del(cam->camera);
            cam->camera = NULL;
        }
        return err;
    }
    cam->jpeg_still = plan.still >= 0;
    cam->exposure_meter = film_still_sensor_has_jpeg(pid);
    mosaico_camera_info_t info = { 0 };
    (void)mosaico_camera_get_info(cam->camera, &info);
    uint16_t photo_w = plan.photo_width, photo_h = plan.photo_height;
    if (cam->jpeg_still) {
        cam->still_width = cam->modes[plan.still].width;
        cam->still_height = cam->modes[plan.still].height;
    } else {
        /* 快门帧就是取景帧：原片是实际取景格式的 4:3 窗口（退路时与规划的不同） */
        film_capture_mode_window((uint16_t)info.width, (uint16_t)info.height, &photo_w, &photo_h);
    }
    ESP_LOGI(TAG, "camera ready: %s (pid 0x%04x) preview %lux%lu @%lu fps x%u buffers, photo %ux%u via %s%s%s",
             film_still_sensor_name(pid), pid, (unsigned long)info.width, (unsigned long)info.height,
             (unsigned long)info.frame_rate, (unsigned)info.buffer_count, (unsigned)photo_w, (unsigned)photo_h,
             cam->jpeg_still ? "JPEG" : "viewfinder frame", use_plan ? "" : ", sensor default format",
             allow_unidentified ? ", unidentified board" : "");
    /* 设置页"关于"：传感器与照片分辨率（用户关心的是拍出来多大） */
    char text[FILM_CAMERA_STATUS_LEN];
    snprintf(text, sizeof(text), "%s %ux%u%s", film_still_sensor_name(pid), (unsigned)photo_w, (unsigned)photo_h,
             allow_unidentified ? " (NO ID)" : "");
    set_status(cam, text);
    return ESP_OK;
}

/**
 * 连接相机，兼容两种传感器与两种常见状况：
 *   - 模块 EEPROM 描述符无效（没烧录或读坏）：识别到有模块在位时放开 allow_unidentified，
 *     是否真是相机由后面的传感器 ID 把关（认不出会失败，不会误开别的板子）；
 *   - 选定的格式开不起来：退到传感器的 Kconfig 默认格式与默认缓冲个数。
 */
static esp_err_t camera_attach(film_camera_handle_t cam)
{
    mosaico_module_mgr_info_t slot = { 0 };
    const bool bad_descriptor = mosaico_module_mgr_get_info(MOSAICO_MODULE_MGR_SLOT_LEFT, &slot) == ESP_OK &&
                                slot.presence == MOSAICO_MODULE_PRESENCE_PRESENT &&
                                slot.descriptor_state == MOSAICO_MODULE_DESCRIPTOR_INVALID;
    esp_err_t err = attach_once(cam, bad_descriptor, true);
    /*
     * 格式类错误才退：esp_video 的 VIDIOC_S_FMT / S_SENSOR_FMT 拒绝时 mosaico_camera 映射成 ESP_FAIL，
     * 协商后不一致是 ESP_ERR_NOT_SUPPORTED，缓冲申请不到是 ESP_ERR_NO_MEM。模块不存在等其它错误不退。
     */
    if (err == ESP_ERR_NOT_SUPPORTED || err == ESP_FAIL || err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NO_MEM) {
        ESP_LOGW(TAG, "planned camera mode failed (%s), trying the sensor default", esp_err_to_name(err));
        err = attach_once(cam, bad_descriptor, false);
    }
    if (err == ESP_OK) {
        set_ready(cam, true);
        return ESP_OK;
    }
    diagnose_failure(cam, err);
    return err;
}

static void camera_detach(film_camera_handle_t cam)
{
    set_ready(cam, false);
    stream_stop(cam);
    (void)mosaico_camera_del(cam->camera);
    cam->camera = NULL;
    set_status(cam, "DISCONNECTED");
    ESP_LOGW(TAG, "camera lost");
}

/* ---------------------------------------------------------------- 取景一帧 */

#ifdef CONFIG_FILM_PERF_TRACE
/** 取景分段耗时：采集等待 / 转换 / 到界面取走的延迟 / 丢帧；并附带内存与各任务 CPU 占用 */
static void perf_report_viewfinder(film_camera_handle_t cam, float seconds)
{
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    const uint32_t acquired = cam->perf_acquired;
    const float latency_ms = acquired ? (float)cam->perf_latency_us / 1000.0f / (float)acquired : 0.0f;
    const uint32_t dropped = cam->perf_dropped;
    cam->perf_latency_us = 0;
    cam->perf_acquired = 0;
    cam->perf_dropped = 0;
    xSemaphoreGive(cam->lock);
    FILM_PERF_LOG("vf",
                  "fps %.1f | capture_wait %.1f ms | convert %.1f ms | ui_pickup_latency %.1f ms "
                  "(ui took %u frames) | dropped_no_slot %u",
                  cam->stat_frames / seconds,
                  cam->perf_wait_frames ? (float)cam->perf_wait_us / 1000.0f / (float)cam->perf_wait_frames : 0.0f,
                  cam->stat_frames ? (float)cam->stat_convert_us / 1000.0f / (float)cam->stat_frames : 0.0f,
                  latency_ms, (unsigned)acquired, (unsigned)dropped);
    cam->perf_wait_us = 0;
    cam->perf_wait_frames = 0;
    film_perf_mem("viewfinder");
    film_perf_cpu_report("viewfinder");
}
#endif

static void log_stats(film_camera_handle_t cam)
{
    const int64_t now = esp_timer_get_time();
    if (cam->stat_since == 0) {
        cam->stat_since = now;
        film_perf_cpu_reset();
    } else if (now - cam->stat_since >= STATS_INTERVAL_US) {
        const float seconds = (float)(now - cam->stat_since) / 1e6f;
        ESP_LOGI(TAG, "preview %.1f fps, convert %.1f ms, PSRAM free %u KB", cam->stat_frames / seconds,
                 cam->stat_frames ? (float)cam->stat_convert_us / 1000.0f / (float)cam->stat_frames : 0.0f,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
#ifdef CONFIG_FILM_PERF_TRACE
        perf_report_viewfinder(cam, seconds);
#endif
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
/** 胶卷取景外观：开关关闭（或表没分配出来）返回 NULL；胶卷或曝光变了才重建（几十微秒） */
static const film_viewfinder_look_t *preview_look(film_camera_handle_t cam, const film_preview_config_t *pv)
{
    if (!pv->film_look || !cam->look) {
        return NULL;
    }
    const int ev_centi = (int)lroundf(pv->exposure_ev * 100.0f);
    if (cam->look_film != (int)pv->film || cam->look_ev_centi != ev_centi) {
        film_look_t look;
        if (film_filter_look(pv->film, pv->exposure_ev, &look) != ESP_OK) {
            return NULL;
        }
        film_viewfinder_look_set(cam->look, &look);
        cam->look_film = (int)pv->film;
        cam->look_ev_centi = ev_centi;
    }
    return cam->look;
}

static esp_err_t preview_step(film_camera_handle_t cam, const film_preview_config_t *pv, bool convert)
{
    mosaico_camera_frame_t frame = { 0 };
#ifdef CONFIG_FILM_PERF_TRACE
    const int64_t wait_t0 = esp_timer_get_time();
#endif
    ESP_RETURN_ON_ERROR(mosaico_camera_get_frame(cam->camera, &frame), TAG, "get frame");
    set_frame_wait(cam, esp_timer_get_time());
#ifdef CONFIG_FILM_PERF_TRACE
    cam->perf_wait_us += esp_timer_get_time() - wait_t0;
    ++cam->perf_wait_frames;
#endif
    const int slot = convert ? pick_write_slot(cam) : -1;
#ifdef CONFIG_FILM_PERF_TRACE
    if (convert && slot < 0) {
        ++cam->perf_dropped;
    }
#endif
    if (slot >= 0) {
        const int64_t t0 = esp_timer_get_time();
        const film_viewfinder_source_t full = {
            .uyvy = frame.data, .width = frame.width, .height = frame.height, .stride = frame.bytes_per_line,
        };
        const film_viewfinder_source_t src = film_viewfinder_window_43(&full);   /* 16:9 传感器取中间 4:3 */
        const esp_err_t err = film_viewfinder_convert_rot_look(&src, pv->instant, pv->crop_43, pv->rot,
                                                                      preview_look(cam, pv), cam->slots[slot].pixels);
        if (err == ESP_OK) {
            uint16_t vw, vh;
            film_viewfinder_size_rot(pv->instant, pv->crop_43, pv->rot, &vw, &vh);
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
 * 复制下一帧取景的 4:3 窗口（640×480 时约 600 KB，960×720 时约 1.3 MB；最多等一帧）。暗房拿它在几百毫秒内
 * 冲出屏幕尺寸的小样，不必等 JPEG 原片几秒钟的软件解码；无 JPEG 的传感器它就是原片。拿不到就返回空来源。
 */
static film_viewfinder_source_t grab_proof(film_camera_handle_t cam)
{
    film_viewfinder_source_t proof = { 0 };
    mosaico_camera_frame_t frame = { 0 };
    if (!cam->streaming || mosaico_camera_get_frame(cam->camera, &frame) != ESP_OK) {
        return proof;
    }
    const size_t stride = frame.bytes_per_line ? frame.bytes_per_line : frame.width * 2u;
    const film_viewfinder_source_t full = {
        .uyvy = frame.data, .width = frame.width, .height = frame.height, .stride = stride,
    };
    /* 与取景同一个 4:3 窗口，按窗口紧凑复制：小样（和无 JPEG 传感器的原片）都以它为准 */
    const film_viewfinder_source_t window = film_viewfinder_window_43(&full);
    const size_t row_bytes = (size_t)window.width * 2u;
    const size_t bytes = row_bytes * window.height;
    uint8_t *copy = stride * frame.height <= frame.size ? heap_caps_malloc(bytes, BUFFER_CAPS) : NULL;
    if (copy) {
        for (uint32_t y = 0; y < window.height; ++y) {
            memcpy(copy + (size_t)y * row_bytes, window.uyvy + (size_t)y * stride, row_bytes);
        }
        proof = (film_viewfinder_source_t){
            .uyvy = copy, .width = window.width, .height = window.height, .stride = row_bytes,
        };
    }
    (void)mosaico_camera_return_frame(cam->camera, &frame);
    film_perf_mark(FILM_PERF_PROOF_GRABBED);
    return proof;
}

/** 取景帧 4:3 窗口的平均亮度（稀疏取 Y） */
static uint32_t frame_luma(const mosaico_camera_frame_t *frame)
{
    const size_t stride = frame->bytes_per_line ? frame->bytes_per_line : frame->width * 2u;
    const film_viewfinder_source_t full = {
        .uyvy = frame->data, .width = frame->width, .height = frame->height, .stride = stride,
    };
    const film_viewfinder_source_t win = film_viewfinder_window_43(&full);
    uint64_t sum = 0;
    uint32_t count = 0;
    for (uint32_t y = 0; y < win.height; y += LUMA_SAMPLE_STEP) {
        const uint8_t *row = win.uyvy + (size_t)y * stride;
        for (uint32_t x = 0; x < win.width; x += LUMA_SAMPLE_STEP) {
            sum += row[x * 2u + 1u];   /* UYVY：U Y0 V Y1 */
            ++count;
        }
    }
    return count ? (uint32_t)(sum / count) : 0;
}

/**
 * 场景读数，用来决定点不点灯、点灯后等多久：
 *   OV3640 读自动曝光寄存器（film_still_exposure_index，越大越暗）；next_frame 时先等过一帧；
 *   其余传感器取下一帧取景的平均亮度（越小越暗）。
 */
static esp_err_t meter_read(film_camera_handle_t cam, bool next_frame, uint32_t *ret)
{
    if (cam->exposure_meter) {
        if (next_frame) {
            ESP_RETURN_ON_ERROR(mosaico_camera_discard_frames(cam->camera, 1), TAG, "frame");
        }
        return film_still_exposure_index(ret);
    }
    mosaico_camera_frame_t frame = { 0 };
    ESP_RETURN_ON_ERROR(mosaico_camera_get_frame(cam->camera, &frame), TAG, "frame");
    *ret = frame_luma(&frame);
    (void)mosaico_camera_return_frame(cam->camera, &frame);
    return ESP_OK;
}

static bool meter_dark(film_camera_handle_t cam, uint32_t value)
{
    return cam->exposure_meter ? value >= FLASH_FIRE_INDEX : value < FLASH_FIRE_LUMA;
}

/** 点灯后让取景继续出帧（取回即还），直到场景读数稳定或超时 */
static void flash_settle(film_camera_handle_t cam)
{
    const int64_t t0 = esp_timer_get_time();
    uint32_t prev = 0;
    uint32_t value = 0;
    int frames = 0;
    while (esp_timer_get_time() - t0 < FLASH_SETTLE_MAX_US) {
        const esp_err_t err = meter_read(cam, true, &value);
        ++frames;
        if (err != ESP_OK) {
            continue;   /* 读数偶尔读不出（SCCB 与触摸共用总线），整个等待有超时兜底 */
        }
        const uint32_t diff = value > prev ? value - prev : prev - value;
        if (frames >= FLASH_SETTLE_MIN_FRAMES && diff * 8u <= prev) {
            break;
        }
        prev = value;
    }
    ESP_LOGI(TAG, "flash: %s settled to %" PRIu32 " after %d frames, %lld ms",
             cam->exposure_meter ? "exposure" : "luma", value, frames, (long long)((esp_timer_get_time() - t0) / 1000));
}

/**
 * 自动闪光：取景测得够暗才点灯，然后等取景曝光在灯光下收敛，小样与快门帧就都按灯光下的曝光来拍
 * （暗房按小样对齐原片的亮度与颜色，两者必须在同一种光线下）。返回 true 表示灯已点亮，调用方负责关灯。
 */
static bool flash_prepare(film_camera_handle_t cam, const film_shot_t *shot)
{
    if (!shot->flash_auto || !cam->streaming) {
        return false;
    }
    uint32_t value = 0;
    if (meter_read(cam, false, &value) != ESP_OK) {
        ESP_LOGW(TAG, "flash: scene unreadable, shooting without flash");
        return false;
    }
    const bool dark = meter_dark(cam, value);
    if (cam->exposure_meter) {
        ESP_LOGI(TAG, "flash: exposure index %" PRIu32 " (fires at %u) -> %s", value, (unsigned)FLASH_FIRE_INDEX,
                 dark ? "fire" : "skip");
    } else {
        ESP_LOGI(TAG, "flash: luma %" PRIu32 " (fires below %u) -> %s", value, (unsigned)FLASH_FIRE_LUMA,
                 dark ? "fire" : "skip");
    }
    if (!dark) {
        return false;
    }
    if (mosaico_camera_flash_trigger(cam->camera) != ESP_OK) {
        ESP_LOGW(TAG, "flash: LED on failed, shooting without flash");
        return false;
    }
    flash_settle(cam);
    return true;
}

static void capture(film_camera_handle_t cam, const film_shot_t *shot)
{
    film_still_t still;
    const bool flash = flash_prepare(cam, shot);
    const film_viewfinder_source_t proof = grab_proof(cam);
    /* 取景流在暂停期间一直关着（驱动帧缓冲让给暗房），由主循环在恢复时重开 */
    stream_stop(cam);
    film_perf_mark(FILM_PERF_STREAM_STOPPED);
    esp_err_t err;
    if (cam->jpeg_still) {
        err = film_still_capture(cam->still_width, cam->still_height, &still);
    } else {
        /* 没有 JPEG 的传感器：快门帧就是刚复制的取景帧，暗房把它编成原片（film_still_t.from_frame） */
        still = (film_still_t){ .from_frame = true };
        err = proof.uyvy ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (flash) {
        (void)mosaico_camera_flash_stop(cam->camera);
        /* 补光灯下的白平衡不能拿去修正环境光下的增益：awb 全 0 时暗房不做白平衡学习 */
        still.awb = (film_still_awb_t){ 0 };
    }
    xSemaphoreTake(cam->lock, portMAX_DELAY);
    cam->capturing = false;
    xSemaphoreGive(cam->lock);
    cam->config.on_still(cam->config.ctx, shot, &still, &proof, err);
    /* 栈余量：拍摄后重启时用来排除栈溢出 */
    ESP_LOGI(TAG, "capture done, stack free min %u bytes", (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void camera_task(void *arg)
{
    film_camera_handle_t cam = arg;
    int frame_errors = 0;
    bool logged_missing = false;
    uint32_t retry_ms = RETRY_DELAY_MS;
    for (;;) {
        if (!cam->camera) {
            /*
             * 等界面要取景（开机动画结束）、暗房也没在冲洗时再连接：连接要开一次流，
             * 取景格式又按连接时的 PSRAM 余量选，动画解码器或冲洗缓冲还在时测得的余量偏小，会白白退到小格式。
             */
            xSemaphoreTake(cam->lock, portMAX_DELAY);
            const bool want = cam->preview_wanted && !cam->hold;
            xSemaphoreGive(cam->lock);
            if (!want) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HOLD_POLL_MS));
                continue;
            }
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
        /* 界面暂停取景（离开取景页、屏幕调暗）：先只停转换，久了再关流 */
        if (!paused) {
            cam->paused_since = 0;
        } else if (cam->paused_since == 0) {
            cam->paused_since = esp_timer_get_time();
        } else if (cam->streaming && esp_timer_get_time() - cam->paused_since >= PAUSE_STREAM_OFF_US) {
            stream_stop(cam);
            ESP_LOGI(TAG, "preview paused: stream off");
        }
        if (paused && !cam->streaming) {
            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PAUSED_POLL_MS));
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
    heap_caps_free(cam->look);
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
    cam->look_film = -1;
    /* 取景外观表每帧被读 17 万次：放内部 RAM；放不下就不提供外观 */
    cam->look = heap_caps_calloc(1, sizeof(*cam->look), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!cam->look) {
        ESP_LOGW(TAG, "no internal RAM for the film look table; film viewfinder disabled");
    }
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

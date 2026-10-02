#include "film_port_pc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "film_assets.h"
#include "film_studio.h"
#include "ppm_io.h"

#define EVENT_QUEUE_LEN     16
#define SCREEN_SLOTS        2
#define STRIP_ROWS          16
#define PREVIEW_PERIOD_MS   80      /*!< 取景重冲间隔：颗粒在变，像真实取景 */
#define SHUTTER_DELAY_MS    120
#define PROGRESS_STEP       40      /*!< 进度事件的最小间隔（千分比） */
#define DEVELOP_DELAY_ENV   "FILM_SIM_DEVELOP_DELAY_MS"   /*!< 每个进度事件后额外等多久，模拟真机的慢冲洗 */
#define ACCEL_POLL_MS       500
#define BATTERY_POLL_MS     500
#define BATTERY_DEFAULT_PERCENT 82
#define PATH_LEN            256

/* 模拟分享：各阶段距开始的时间 */
#define SHARE_READY_MS      900
#define SHARE_VISIT_MS      4000
#define SHARE_SAVE_MS       7000
#define SHARE_HOTSPOT_JOIN_MS 5000

typedef struct {
    film_frame_t frame;
    uint16_t *pixels;
    bool held;
} preview_slot_t;

typedef struct {
    uint16_t *pixels;
    bool busy;
} screen_slot_t;

struct film_port_pc_t {
    film_port_t table;
    char data_dir[PATH_LEN];
    char root[PATH_LEN];
    bool storage_ok;
    ppm_image_t sensor;

    /* 取景（界面线程独占） */
    film_filter_handle_t preview_filter;
    film_darkroom_handle_t preview_room;
    film_preview_config_t preview_cfg;
    preview_slot_t preview[2];
    uint32_t preview_seq;
    uint64_t preview_at;
    bool preview_dirty;
    bool preview_paused;        /*!< 界面请求暂停：不再出新帧（检验界面在定格期间不依赖新帧） */

    /* 冲洗工作线程；以下字段由 lock 保护 */
    pthread_t worker;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool quit;
    bool job_pending;
    film_shot_t job;
    bool job_redevelop;
    film_event_t events[EVENT_QUEUE_LEN];
    size_t ev_head, ev_count;
    screen_slot_t screens[SCREEN_SLOTS];
    film_filter_handle_t shot_filter;    /*!< 工作线程独占 */
    film_darkroom_handle_t shot_room;

    /* 模拟分享（界面线程） */
    film_share_phase_t share_phase;
    bool share_hotspot;
    uint64_t share_t0;
    size_t share_count;
    film_share_status_t share_reported;

    float accel[3];
    uint64_t accel_at;
    film_battery_t battery;
    bool battery_valid;
    uint64_t battery_at;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void sleep_ms(unsigned ms)
{
    const struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void *heap_alloc(size_t bytes, void *ctx)
{
    (void)ctx;
    return malloc(bytes);
}

static void heap_free(void *ptr, void *ctx)
{
    (void)ctx;
    free(ptr);
}

/** mkdir -p */
static bool make_dirs(const char *path)
{
    char buf[PATH_LEN];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST;
}

/* ---------------------------------------------------------------- 原片来源 */

typedef struct {
    const ppm_image_t *image;
    int row;
} mem_source_t;

static esp_err_t mem_read(void *ctx, const uint8_t **ret_rows, size_t *ret_stride, int *ret_count)
{
    mem_source_t *m = ctx;
    const int left = m->image->height - m->row;
    const int n = left < STRIP_ROWS ? left : STRIP_ROWS;
    *ret_rows = m->image->pixels + (size_t)m->row * m->image->width * 3;
    *ret_stride = (size_t)m->image->width * 3;
    *ret_count = n;
    m->row += n;
    return ESP_OK;
}

static film_darkroom_source_t mem_source(mem_source_t *m, const ppm_image_t *image)
{
    m->image = image;
    m->row = 0;
    return (film_darkroom_source_t) {
        .width = (uint16_t)image->width, .height = (uint16_t)image->height, .ctx = m, .read = mem_read,
    };
}

/* ---------------------------------------------------------------- 事件 */

static void push_event_locked(film_port_pc_handle_t p, const film_event_t *ev)
{
    if (p->ev_count == EVENT_QUEUE_LEN) {
        fprintf(stderr, "film_port_pc: event queue full, dropping type %d\n", ev->type);
        return;
    }
    p->events[(p->ev_head + p->ev_count) % EVENT_QUEUE_LEN] = *ev;
    ++p->ev_count;
}

static void push_event(film_port_pc_handle_t p, const film_event_t *ev)
{
    pthread_mutex_lock(&p->lock);
    push_event_locked(p, ev);
    pthread_mutex_unlock(&p->lock);
}

static void share_tick(film_port_pc_handle_t p);

static bool pc_poll_event(void *ctx, film_event_t *ret_event)
{
    film_port_pc_handle_t p = ctx;
    share_tick(p);
    pthread_mutex_lock(&p->lock);
    const bool have = p->ev_count > 0;
    if (have) {
        *ret_event = p->events[p->ev_head];
        p->ev_head = (p->ev_head + 1) % EVENT_QUEUE_LEN;
        --p->ev_count;
    }
    pthread_mutex_unlock(&p->lock);
    return have;
}

/* ---------------------------------------------------------------- 冲洗工作线程 */

static esp_err_t write_ppm_jpeg(void *ctx, film_darkroom_image_t *image, const char *path, uint32_t *ret_bytes)
{
    (void)ctx;
    /* 成片行跨度等于宽度 × 3，可以直接当紧凑 PPM 写出 */
    const ppm_image_t out = { .pixels = image->pixels, .width = image->width, .height = image->height };
    if (!ppm_write(path, &out)) {
        return ESP_FAIL;
    }
    struct stat st;
    *ret_bytes = stat(path, &st) == 0 ? (uint32_t)st.st_size : 0;
    return ESP_OK;
}

/** 取一块空闲的屏幕图缓冲（界面归还之前一直占用）；lock 已持有 */
static screen_slot_t *take_screen_locked(film_port_pc_handle_t p)
{
    for (;;) {
        for (int i = 0; i < SCREEN_SLOTS; ++i) {
            if (!p->screens[i].busy) {
                p->screens[i].busy = true;
                return &p->screens[i];
            }
        }
        if (p->quit) {
            return NULL;
        }
        pthread_cond_wait(&p->wake, &p->lock);
    }
}

/* 冲洗进度 → PROGRESS 事件（与真机一样按阶段折算成千分比，冲完前最多 999） */
typedef struct {
    film_port_pc_handle_t port;
    uint32_t photo_id;
    uint16_t sent;
    unsigned delay_ms;
} pc_progress_t;

static void on_progress(void *ctx, film_stage_t stage, float done)
{
    pc_progress_t *pp = ctx;
    const float total = ((float)stage + done) / (float)FILM_STAGE_COUNT;
    const uint16_t value = (uint16_t)(total * (FILM_PROGRESS_DONE - 1));
    if (value < pp->sent + PROGRESS_STEP) {
        return;
    }
    /* 模拟慢冲洗：按进度增量折算等待（暗房报进度的粒度不均匀，按次数等会忽快忽慢） */
    const unsigned wait = pp->delay_ms * (unsigned)(value - pp->sent) / PROGRESS_STEP;
    pp->sent = value;
    const film_event_t ev = { .type = FILM_EVT_PROGRESS, .photo_id = pp->photo_id, .progress = value };
    push_event(pp->port, &ev);
    if (wait) {
        sleep_ms(wait);
    }
}

/** 小样：真机用快门前最后一帧取景冲屏幕尺寸，这里直接用模拟传感器图 */
static bool develop_proof(film_port_pc_handle_t p, const film_shot_t *shot, screen_slot_t *slot,
                          film_tone_stats_t *ret_stats)
{
    film_shot_t proof = *shot;
    proof.preview_only = true;
    mem_source_t m;
    const film_darkroom_source_t source = mem_source(&m, &p->sensor);
    const film_studio_config_t studio = { .darkroom = p->shot_room, .ret_stats = ret_stats };
    film_studio_result_t result;
    if (film_studio_process(&studio, &proof, &source, slot->pixels, &result) != ESP_OK) {
        return false;
    }
    const film_event_t developed = {
        .type = FILM_EVT_DEVELOPED, .photo_id = shot->photo_id, .screen = slot->pixels, .width = result.screen_w,
        .height = result.screen_h, .preview = false, .film = shot->film, .instant = shot->instant,
    };
    push_event(p, &developed);
    return true;
}

static esp_err_t run_job(film_port_pc_handle_t p, const film_shot_t *shot, bool redevelop, screen_slot_t *slot,
                         film_studio_result_t *result, pc_progress_t *progress, const film_tone_stats_t *match)
{
    char raw_path[FILM_PATH_MAX];
    ppm_image_t loaded = { 0 };
    const ppm_image_t *src = &p->sensor;
    if (redevelop) {
        char from[FILM_PATH_MAX];
        film_library_path(p->root, shot->source_id, FILM_FILE_RAW, from, sizeof(from));
        if (!ppm_read(from, &loaded)) {
            return ESP_ERR_NOT_FOUND;
        }
        src = &loaded;
        if (!shot->preview_only) {
            film_library_path(p->root, shot->photo_id, FILM_FILE_RAW, raw_path, sizeof(raw_path));
            if (film_studio_copy_file(from, raw_path) != ESP_OK) {
                free(loaded.pixels);
                return ESP_FAIL;
            }
        }
    } else {
        sleep_ms(SHUTTER_DELAY_MS);
        const film_event_t done = { .type = FILM_EVT_SHUTTER_DONE, .photo_id = shot->photo_id };
        push_event(p, &done);
        film_library_path(p->root, shot->photo_id, FILM_FILE_RAW, raw_path, sizeof(raw_path));
        if (!ppm_write(raw_path, &p->sensor)) {
            return ESP_FAIL;
        }
    }
    mem_source_t m;
    const film_darkroom_source_t source = mem_source(&m, src);
    const film_studio_config_t studio = {
        .darkroom = p->shot_room, .root = p->root, .write_jpeg = write_ppm_jpeg, .ctx = p,
        .progress = shot->preview_only ? NULL : on_progress, .progress_ctx = progress,
        .match = match && match->valid ? match : NULL,
    };
    esp_err_t err = film_studio_process(&studio, shot, &source, slot->pixels, result);
    free(loaded.pixels);
    if (err != ESP_OK && !shot->preview_only) {
        remove(raw_path);
    }
    return err;
}

static void *worker_main(void *arg)
{
    film_port_pc_handle_t p = arg;
    pthread_mutex_lock(&p->lock);
    for (;;) {
        while (!p->job_pending && !p->quit) {
            pthread_cond_wait(&p->wake, &p->lock);
        }
        if (p->quit) {
            break;
        }
        const film_shot_t shot = p->job;
        const bool redevelop = p->job_redevelop;
        screen_slot_t *slot = take_screen_locked(p);
        pthread_mutex_unlock(&p->lock);
        if (!slot) {
            pthread_mutex_lock(&p->lock);
            break;
        }

        const uint64_t t0 = now_ms();
        const char *delay = getenv(DEVELOP_DELAY_ENV);
        pc_progress_t progress = { .port = p, .photo_id = shot.photo_id, .delay_ms = delay ? (unsigned)atoi(delay) : 0 };
        /* 新拍的照片先出小样（第二块屏幕图缓冲，界面拷走后就还回来） */
        bool proof_sent = false;
        film_tone_stats_t proof_stats = { .valid = false };
        if (!redevelop) {
            pthread_mutex_lock(&p->lock);
            screen_slot_t *proof_slot = take_screen_locked(p);
            pthread_mutex_unlock(&p->lock);
            if (proof_slot) {
                proof_sent = develop_proof(p, &shot, proof_slot, &proof_stats);
                if (!proof_sent) {
                    pthread_mutex_lock(&p->lock);
                    proof_slot->busy = false;
                    pthread_cond_broadcast(&p->wake);
                    pthread_mutex_unlock(&p->lock);
                }
            }
        }
        film_studio_result_t result;
        const esp_err_t err = run_job(p, &shot, redevelop, slot, &result, &progress, &proof_stats);
        const uint32_t event_id = shot.preview_only ? shot.source_id : shot.photo_id;
        fprintf(stderr, "film_port_pc: %s %u film=%d instant=%d -> %s in %u ms\n",
                redevelop ? (shot.preview_only ? "preview" : "redevelop") : "shoot", (unsigned)event_id,
                (int)shot.film, (int)shot.instant, err == ESP_OK ? "ok" : "FAILED", (unsigned)(now_ms() - t0));

        pthread_mutex_lock(&p->lock);
        if (err == ESP_OK) {
            /* 与真机同序：（没出小样才给）DEVELOPED → PROGRESS(DONE) → SAVED */
            if (proof_sent) {
                slot->busy = false;
                pthread_cond_broadcast(&p->wake);
            } else {
                const film_event_t developed = {
                    .type = FILM_EVT_DEVELOPED, .photo_id = event_id, .screen = slot->pixels,
                    .width = result.screen_w, .height = result.screen_h, .preview = shot.preview_only,
                    .film = shot.film, .instant = shot.instant,
                };
                push_event_locked(p, &developed);
            }
            if (!shot.preview_only) {
                const film_event_t done = {
                    .type = FILM_EVT_PROGRESS, .photo_id = event_id, .progress = FILM_PROGRESS_DONE,
                };
                push_event_locked(p, &done);
                const film_event_t saved = { .type = FILM_EVT_SAVED, .photo_id = event_id, .meta = result.meta };
                push_event_locked(p, &saved);
            }
        } else {
            slot->busy = false;
            const film_event_t failed = { .type = FILM_EVT_SHOT_FAILED, .err = err, .photo_id = event_id };
            push_event_locked(p, &failed);
        }
        p->job_pending = false;
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

static esp_err_t submit(film_port_pc_handle_t p, const film_shot_t *shot, bool redevelop)
{
    if (!p->storage_ok && !shot->preview_only) {
        return FILM_ERR_STORAGE_FULL;
    }
    pthread_mutex_lock(&p->lock);
    const bool busy = p->job_pending;
    if (!busy) {
        p->job = *shot;
        p->job_redevelop = redevelop;
        p->job_pending = true;
        pthread_cond_broadcast(&p->wake);
    }
    pthread_mutex_unlock(&p->lock);
    return busy ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t pc_shoot(void *ctx, const film_shot_t *shot)
{
    film_port_pc_handle_t p = ctx;
    return p->sensor.pixels ? submit(p, shot, false) : FILM_ERR_NO_CAMERA;
}

static esp_err_t pc_redevelop(void *ctx, const film_shot_t *shot)
{
    return submit(ctx, shot, true);
}

static void pc_release_result(void *ctx, const uint16_t *screen)
{
    film_port_pc_handle_t p = ctx;
    pthread_mutex_lock(&p->lock);
    for (int i = 0; i < SCREEN_SLOTS; ++i) {
        if (p->screens[i].pixels == screen) {
            p->screens[i].busy = false;
        }
    }
    pthread_cond_broadcast(&p->wake);
    pthread_mutex_unlock(&p->lock);
}

/* ---------------------------------------------------------------- 取景 */

static const film_frame_t *pc_preview_acquire(void *ctx)
{
    film_port_pc_handle_t p = ctx;
    const uint64_t t = now_ms();
    if (!p->sensor.pixels || p->preview_paused || (!p->preview_dirty && t - p->preview_at < PREVIEW_PERIOD_MS)) {
        return NULL;
    }
    preview_slot_t *slot = !p->preview[0].held ? &p->preview[0] : !p->preview[1].held ? &p->preview[1] : NULL;
    if (!slot) {
        return NULL;
    }
    /* 与真机一致：取景只显示原片（不加胶卷效果），胶卷在冲洗成片时才生效 */
    const film_darkroom_job_t job = {
        .film = FILM_ID_COUNT, .seed = (uint32_t)t,
        .instant = p->preview_cfg.instant, .size = FILM_DARKROOM_VIEWFINDER,
    };
    mem_source_t m;
    const film_darkroom_source_t source = mem_source(&m, &p->sensor);
    film_darkroom_image_t image;
    if (film_darkroom_develop(p->preview_room, &job, &source, &image) != ESP_OK) {
        return NULL;
    }
    uint32_t scratch[3 * FILM_DARKROOM_SCREEN_MAX_W];
    film_darkroom_downscale_565(&image, slot->pixels, image.width, image.height, scratch);
    slot->frame = (film_frame_t) {
        .pixels = slot->pixels, .width = image.width, .height = image.height, .seq = ++p->preview_seq,
    };
    film_darkroom_release(p->preview_room, &image);
    slot->held = true;
    p->preview_at = t;
    p->preview_dirty = false;
    return &slot->frame;
}

static void pc_preview_release(void *ctx, const film_frame_t *frame)
{
    film_port_pc_handle_t p = ctx;
    for (int i = 0; i < 2; ++i) {
        if (&p->preview[i].frame == frame) {
            p->preview[i].held = false;
        }
    }
}

static void pc_preview_config(void *ctx, const film_preview_config_t *config)
{
    film_port_pc_handle_t p = ctx;
    p->preview_cfg = *config;
    p->preview_dirty = true;
}

static bool pc_camera_ready(void *ctx)
{
    return ((film_port_pc_handle_t)ctx)->sensor.pixels != NULL;
}

static void pc_preview_pause(void *ctx, bool paused)
{
    film_port_pc_handle_t p = ctx;
    p->preview_paused = paused;
    if (!paused) {
        p->preview_dirty = true;   /* 恢复后立刻出一帧，和真机"下一帧就有画面"一致 */
    }
    fprintf(stderr, "film_port_pc: preview %s\n", paused ? "paused" : "resumed");
}

/* ---------------------------------------------------------------- 存储与设置 */

static const char *pc_storage_root(void *ctx)
{
    film_port_pc_handle_t p = ctx;
    return p->storage_ok ? p->root : NULL;
}

static bool pc_settings_load(void *ctx, void *data, size_t len)
{
    film_port_pc_handle_t p = ctx;
    char path[PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/settings.bin", p->data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    const bool ok = fread(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static void pc_settings_save(void *ctx, const void *data, size_t len)
{
    film_port_pc_handle_t p = ctx;
    char path[PATH_LEN + 16];
    snprintf(path, sizeof(path), "%s/settings.bin", p->data_dir);
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

/* ---------------------------------------------------------------- 模拟分享 */

static void share_compute(film_port_pc_handle_t p, film_share_status_t *s)
{
    memset(s, 0, sizeof(*s));
    s->phase = p->share_phase;
    if (p->share_phase == FILM_SHARE_IDLE) {
        return;
    }
    const uint64_t dt = now_ms() - p->share_t0;
    if (dt < SHARE_READY_MS) {
        s->phase = FILM_SHARE_STARTING;
        return;
    }
    if (p->share_hotspot) {
        snprintf(s->ssid, sizeof(s->ssid), "Mosaico-Film-3F2A");
        snprintf(s->password, sizeof(s->password), "film3f2a");
        snprintf(s->host, sizeof(s->host), "192.168.4.1");
        snprintf(s->url, sizeof(s->url), "http://192.168.4.1/s/7k2q9x");
        s->phase = dt < SHARE_HOTSPOT_JOIN_MS ? FILM_SHARE_HOTSPOT_JOIN : FILM_SHARE_HOTSPOT_OPEN;
        return;
    }
    snprintf(s->ssid, sizeof(s->ssid), "MosaicoHome");
    snprintf(s->host, sizeof(s->host), "192.168.1.23");
    snprintf(s->url, sizeof(s->url), "http://192.168.1.23/s/7k2q9x");
    s->phase = FILM_SHARE_LAN;
    s->visits = dt >= SHARE_VISIT_MS ? 1 : 0;
    s->downloads = dt >= SHARE_SAVE_MS ? (uint16_t)p->share_count : 0;
}

static esp_err_t pc_share_start(void *ctx, const uint32_t *ids, size_t count, bool force_hotspot)
{
    film_port_pc_handle_t p = ctx;
    (void)ids;
    p->share_phase = FILM_SHARE_STARTING;
    p->share_hotspot = force_hotspot;
    p->share_count = count;
    p->share_t0 = now_ms();
    memset(&p->share_reported, 0, sizeof(p->share_reported));
    fprintf(stderr, "film_port_pc: share %zu photo(s)%s\n", count, force_hotspot ? " via hotspot" : "");
    return ESP_OK;
}

static void pc_share_stop(void *ctx)
{
    film_port_pc_handle_t p = ctx;
    p->share_phase = FILM_SHARE_IDLE;
}

static void pc_share_status(void *ctx, film_share_status_t *ret_status)
{
    share_compute(ctx, ret_status);
}

/** 分享状态变化时补发事件（界面线程上调用） */
static void share_tick(film_port_pc_handle_t p)
{
    film_share_status_t s;
    share_compute(p, &s);
    if (memcmp(&s, &p->share_reported, sizeof(s)) != 0) {
        p->share_reported = s;
        const film_event_t ev = { .type = FILM_EVT_SHARE };
        push_event(p, &ev);
    }
}

/* ---------------------------------------------------------------- 反馈与传感器 */

static void pc_feedback(void *ctx, film_feedback_t kind)
{
    static const char *const names[] = { "shutter", "detent", "click", "lever", "shake", "error" };
    (void)ctx;
    if ((unsigned)kind < sizeof(names) / sizeof(names[0]) && kind != FILM_FEEDBACK_DETENT) {
        fprintf(stderr, "film_port_pc: feedback %s\n", names[kind]);
    }
}

/**
 * 重力方向默认竖直握持（y 向下）。模拟器里转动相机：往数据目录写 accel 文件，
 * 例如 `echo "1 0 0" > accel` 表示向右侧倒（横屏转 90°）。
 */
static bool pc_read_accel(void *ctx, float *x, float *y, float *z)
{
    film_port_pc_handle_t p = ctx;
    const uint64_t t = now_ms();
    if (t - p->accel_at >= ACCEL_POLL_MS) {
        p->accel_at = t;
        char path[PATH_LEN + 8];
        snprintf(path, sizeof(path), "%s/accel", p->data_dir);
        FILE *f = fopen(path, "r");
        float v[3];
        if (f && fscanf(f, "%f %f %f", &v[0], &v[1], &v[2]) == 3) {
            memcpy(p->accel, v, sizeof(v));
        }
        if (f) {
            fclose(f);
        }
    }
    *x = p->accel[0];
    *y = p->accel[1];
    *z = p->accel[2];
    return true;
}

static bool pc_wall_time(void *ctx, int64_t *ret_time)
{
    (void)ctx;
    *ret_time = (int64_t)time(NULL);
    return true;
}

/**
 * 默认 82%、未充电。模拟器里改电量：往数据目录写 battery 文件，
 * 例如 `echo "8 0" > battery`（8%、未充电）、`echo "46 1" > battery`（充电中）、
 * `echo "none" > battery`（读不到电量计，指示隐藏）。
 */
static bool pc_read_battery(void *ctx, film_battery_t *ret_battery)
{
    film_port_pc_handle_t p = ctx;
    const uint64_t t = now_ms();
    if (!p->battery_at || t - p->battery_at >= BATTERY_POLL_MS) {
        p->battery_at = t;
        char path[PATH_LEN + 8];
        snprintf(path, sizeof(path), "%s/battery", p->data_dir);
        FILE *f = fopen(path, "r");
        if (f) {
            unsigned percent = 0;
            unsigned charging = 0;
            const int n = fscanf(f, "%u %u", &percent, &charging);
            p->battery_valid = n >= 1;
            p->battery = (film_battery_t){ .percent = (uint8_t)(percent > 100 ? 100 : percent),
                                           .charging = n == 2 && charging != 0 };
            fclose(f);
        }
    }
    *ret_battery = p->battery;
    return p->battery_valid;
}

/* ---------------------------------------------------------------- 生命周期 */

static esp_err_t load_assets(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* 素材在整个进程生命周期内被引用，不释放 */
    uint8_t *data = size > 0 ? malloc((size_t)size) : NULL;
    const bool ok = data && fread(data, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    esp_err_t err = ok ? film_assets_bind(data, (size_t)size) : ESP_FAIL;
    if (err != ESP_OK) {
        free(data);
    }
    return err;
}

esp_err_t film_port_pc_create(const film_port_pc_config_t *config, film_port_pc_handle_t *ret_handle)
{
    esp_err_t err = load_assets(config->assets_path);
    if (err != ESP_OK) {
        fprintf(stderr, "film_port_pc: cannot bind assets %s (0x%x)\n", config->assets_path, err);
        return err;
    }
    film_port_pc_handle_t p = calloc(1, sizeof(*p));
    if (!p) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(p->data_dir, sizeof(p->data_dir), "%s", config->data_dir);
    snprintf(p->root, sizeof(p->root), "%s/DCIM/MOSAICO", config->data_dir);
    p->storage_ok = getenv("FILM_SIM_NO_STORAGE") == NULL && make_dirs(p->root);
    if (getenv("FILM_SIM_NO_CAMERA") == NULL && !ppm_read(config->sensor_ppm, &p->sensor)) {
        fprintf(stderr, "film_port_pc: no sensor sample %s, camera disabled\n", config->sensor_ppm);
    }
    p->accel[1] = 1.0f;
    p->battery = (film_battery_t){ .percent = BATTERY_DEFAULT_PERCENT };
    p->battery_valid = true;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->wake, NULL);

    const film_filter_config_t pf = { .max_width = 480, .max_height = 480 };
    const film_filter_config_t sf = { .max_width = 1440, .max_height = 1440, .parallel = true };
    err = film_filter_create(&pf, &p->preview_filter);
    if (err == ESP_OK) {
        err = film_filter_create(&sf, &p->shot_filter);
    }
    if (err == ESP_OK) {
        const film_darkroom_config_t dc = { .filter = p->preview_filter, .alloc = heap_alloc, .free = heap_free };
        err = film_darkroom_create(&dc, &p->preview_room);
    }
    if (err == ESP_OK) {
        const film_darkroom_config_t dc = { .filter = p->shot_filter, .alloc = heap_alloc, .free = heap_free };
        err = film_darkroom_create(&dc, &p->shot_room);
    }
    for (int i = 0; i < 2 && err == ESP_OK; ++i) {
        p->preview[i].pixels = malloc(sizeof(uint16_t) * FILM_VF_M6_W * FILM_VF_M6_H);
        p->screens[i].pixels = malloc(sizeof(uint16_t) * FILM_STUDIO_SCREEN_PIXELS);
        if (!p->preview[i].pixels || !p->screens[i].pixels) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK && pthread_create(&p->worker, NULL, worker_main, p) != 0) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        film_port_pc_delete(p);
        return err;
    }
    p->table = (film_port_t) {
        .ctx = p,
        .preview_acquire = pc_preview_acquire,
        .preview_release = pc_preview_release,
        .preview_config = pc_preview_config,
        .camera_ready = pc_camera_ready,
        .preview_pause = pc_preview_pause,
        .shoot = pc_shoot,
        .redevelop = pc_redevelop,
        .release_result = pc_release_result,
        .poll_event = pc_poll_event,
        .storage_root = pc_storage_root,
        .settings_load = pc_settings_load,
        .settings_save = pc_settings_save,
        .share_start = pc_share_start,
        .share_stop = pc_share_stop,
        .share_status = pc_share_status,
        .feedback = pc_feedback,
        .read_accel = pc_read_accel,
        .wall_time = pc_wall_time,
        .read_battery = pc_read_battery,
    };
    *ret_handle = p;
    return ESP_OK;
}

void film_port_pc_delete(film_port_pc_handle_t p)
{
    if (!p) {
        return;
    }
    if (p->worker) {
        pthread_mutex_lock(&p->lock);
        p->quit = true;
        pthread_cond_broadcast(&p->wake);
        pthread_mutex_unlock(&p->lock);
        pthread_join(p->worker, NULL);
    }
    film_darkroom_delete(p->preview_room);
    film_darkroom_delete(p->shot_room);
    if (p->preview_filter) {
        film_filter_delete(p->preview_filter);
    }
    if (p->shot_filter) {
        film_filter_delete(p->shot_filter);
    }
    for (int i = 0; i < 2; ++i) {
        free(p->preview[i].pixels);
        free(p->screens[i].pixels);
    }
    free(p->sensor.pixels);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->wake);
    free(p);
}

const film_port_t *film_port_pc_table(film_port_pc_handle_t p)
{
    return &p->table;
}

esp_err_t film_port_pc_set_sensor(film_port_pc_handle_t p, const uint8_t *rgb, int width, int height)
{
    if (!p || !rgb || width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t bytes = (size_t)width * (size_t)height * 3;
    pthread_mutex_lock(&p->lock);
    esp_err_t err = ESP_OK;
    if (p->job_pending) {
        err = ESP_ERR_INVALID_STATE;   /* 工作线程只在 job_pending 期间读样片 */
    } else if (!p->sensor.pixels || p->sensor.width != width || p->sensor.height != height) {
        uint8_t *pixels = malloc(bytes);
        if (!pixels) {
            err = ESP_ERR_NO_MEM;
        } else {
            free(p->sensor.pixels);
            p->sensor = (ppm_image_t){ .pixels = pixels, .width = width, .height = height };
        }
    }
    if (err == ESP_OK) {
        memcpy(p->sensor.pixels, rgb, bytes);
        p->preview_dirty = true;
    }
    pthread_mutex_unlock(&p->lock);
    return err;
}

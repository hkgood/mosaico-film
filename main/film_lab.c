#include "film_lab.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/jpeg_encode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "film_darkroom.h"
#include "film_storage.h"
#include "film_studio.h"
#include "film_writer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "film_lab";

#define LAB_TASK_STACK          8192
#define JOB_QUEUE_LEN           2
#define SCREEN_SLOTS            2       /*!< 一张界面在显示、一张给下一次冲洗 */
#define SCREEN_WAIT_MS          5000
#define FILTER_MAX_SIDE         1440    /*!< film_darkroom 要求滤镜实例至少 1440×1440 */
#define JPEG_QUALITY            92
#define JPEG_QUALITY_FALLBACK   80      /*!< 极少数细节很多的照片超出输出缓冲时，降一档重编 */
#define JPEG_TIMEOUT_MS         2000
#define JPEG_OUT_BYTES_NUM      1       /*!< 输出缓冲按每像素 1/2 字节预留，q92 实际约 0.3 */
#define JPEG_OUT_BYTES_DEN      2
#define WARM_UP_IDLE_MS         6000    /*!< 开机后空闲这么久再预热存储（避开开机动画） */
#define PROOF_STRIP_ROWS        16      /*!< 小样来源每次换算的取景行数 */
#define PROGRESS_STEP           20      /*!< 进度至少涨 2% 才发一次事件 */
#define PROGRESS_MIN_GAP_US     (150 * 1000)
#define PROGRESS_QUEUE_RESERVE  4       /*!< 事件队列剩这么多空位时不再发进度，留给结果事件 */
#define IMAGE_ALIGN             64      /*!< 成片缓冲要直接交给硬件编码器（2D-DMA） */
#define IMAGE_CAPS              (MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)
#define FILE_CAPS               (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/*
 * 全尺寸冲洗各阶段在关键路径上的时间占比（实测：解码+重采样最重），用来把阶段进度换算成总进度。
 * 写盘在后台进行，不计入（WRITE 只剩 .THM/.SCR 的像素复制）。
 */
static const float k_stage_weight[FILM_STAGE_COUNT] = {
    [FILM_STAGE_READ] = 0.78f,
    [FILM_STAGE_FILTER] = 0.11f,
    [FILM_STAGE_FINISH] = 0.01f,
    [FILM_STAGE_PREVIEWS] = 0.06f,
    [FILM_STAGE_ENCODE] = 0.04f,
    [FILM_STAGE_WRITE] = 0.0f,
};

typedef struct {
    film_shot_t shot;
    bool redevelop;
    uint8_t *jpeg;              /*!< 拍摄：原片（任务负责释放）；重新冲洗时为 NULL */
    size_t size;
    film_viewfinder_source_t proof; /*!< 快门前最后一帧取景（uyvy 归任务所有，可能为 NULL） */
    film_tone_stats_t proof_stats;  /*!< 小样原片的通道均值：成片原片对齐到它（见 film_balance.h） */
    film_still_awb_t awb;           /*!< 拍摄所用的白平衡增益（只对新拍的照片有意义） */
} job_t;

struct film_lab_t {
    film_lab_config_t config;
    TaskHandle_t task;
    QueueHandle_t jobs;
    film_writer_handle_t writer;

    /* lock 保护下面这一组 */
    SemaphoreHandle_t lock;
    bool busy;                  /*!< 已预约或有任务在排队/处理 */
    const char *root;
    bool screen_busy[SCREEN_SLOTS];

    SemaphoreHandle_t screen_free;  /*!< 空闲屏幕图缓冲计数 */
    uint16_t *screens[SCREEN_SLOTS];

    /* 只在暗房任务里使用 */
    film_filter_handle_t filter;
    film_darkroom_handle_t darkroom;
    jpeg_encoder_handle_t encoder;
};

/* ---------------------------------------------------------------- 小工具 */

static void push_event(film_lab_handle_t lab, const film_event_t *ev)
{
    if (xQueueSend(lab->config.events, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping type %d", ev->type);
    }
}

static void set_busy(film_lab_handle_t lab, bool busy)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->busy = busy;
    xSemaphoreGive(lab->lock);
}

static const char *get_root(film_lab_handle_t lab)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    const char *root = lab->root;
    xSemaphoreGive(lab->lock);
    return root;
}

static esp_err_t read_file(const char *path, uint8_t **ret_data, size_t *ret_size)
{
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t *data = heap_caps_malloc((size_t)st.st_size, FILE_CAPS);
    ESP_RETURN_ON_FALSE(data, ESP_ERR_NO_MEM, TAG, "read buffer %ld", (long)st.st_size);
    FILE *f = fopen(path, "rb");
    const bool ok = f && fread(data, 1, (size_t)st.st_size, f) == (size_t)st.st_size;
    if (f) {
        fclose(f);
    }
    if (!ok) {
        heap_caps_free(data);
        return ESP_FAIL;
    }
    *ret_data = data;
    *ret_size = (size_t)st.st_size;
    return ESP_OK;
}

static void log_psram(const char *what)
{
    ESP_LOGW(TAG, "%s: PSRAM free %u KB, largest block %u KB", what,
             (unsigned)(heap_caps_get_free_size(IMAGE_CAPS) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(IMAGE_CAPS) / 1024));
}

/* ---------------------------------------------------------------- 原片来源：JPEG 分块解码 */

typedef struct {
    jpeg_dec_handle_t decoder;
    jpeg_dec_io_t io;
    uint8_t *block;
    int blocks_left;
    uint16_t width;
    int64_t decode_us;          /*!< 解码累计耗时（不含重采样），用于耗时分解 */
} jpeg_source_t;

static esp_err_t jpeg_source_read(void *ctx, const uint8_t **ret_rows, size_t *ret_stride, int *ret_count)
{
    jpeg_source_t *s = ctx;
    *ret_stride = (size_t)s->width * 3u;
    if (s->blocks_left <= 0) {
        *ret_count = 0;
        return ESP_OK;
    }
    const int64_t t0 = esp_timer_get_time();
    const jpeg_error_t jerr = jpeg_dec_process(s->decoder, &s->io);
    s->decode_us += esp_timer_get_time() - t0;
    ESP_RETURN_ON_FALSE(jerr == JPEG_ERR_OK, ESP_FAIL, TAG, "decode block: %d", jerr);
    --s->blocks_left;
    *ret_rows = s->block;
    *ret_count = (int)(s->io.out_size / (int)*ret_stride);
    return ESP_OK;
}

static void jpeg_source_close(jpeg_source_t *s)
{
    if (s->decoder) {
        jpeg_dec_close(s->decoder);
    }
    if (s->block) {
        jpeg_free_align(s->block);
    }
    memset(s, 0, sizeof(*s));
}

/* 解码器输出 R、G、B 字节顺序，正是 film_darkroom 要的 */
static esp_err_t jpeg_source_open(jpeg_source_t *s, uint8_t *jpeg, size_t size, film_darkroom_source_t *ret)
{
    memset(s, 0, sizeof(*s));
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB888;
    config.block_enable = true;
    ESP_RETURN_ON_FALSE(jpeg_dec_open(&config, &s->decoder) == JPEG_ERR_OK, ESP_FAIL, TAG, "decoder open");
    s->io.inbuf = jpeg;
    s->io.inbuf_len = (int)size;
    jpeg_dec_header_info_t info = { 0 };
    int block_bytes = 0;
    esp_err_t err = ESP_OK;
    if (jpeg_dec_parse_header(s->decoder, &s->io, &info) != JPEG_ERR_OK ||
        jpeg_dec_get_outbuf_len(s->decoder, &block_bytes) != JPEG_ERR_OK || block_bytes <= 0 ||
        jpeg_dec_get_process_count(s->decoder, &s->blocks_left) != JPEG_ERR_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        s->block = jpeg_calloc_align((size_t)block_bytes, 16);
        err = s->block ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "JPEG source: %s", esp_err_to_name(err));
        jpeg_source_close(s);
        return err;
    }
    s->io.outbuf = s->block;
    s->width = info.width;
    *ret = (film_darkroom_source_t){
        .width = info.width, .height = info.height, .ctx = s, .read = jpeg_source_read,
    };
    return ESP_OK;
}

/* ---------------------------------------------------------------- 小样来源：取景帧 UYVY */

typedef struct {
    film_viewfinder_source_t frame;
    uint8_t *rows;              /*!< PROOF_STRIP_ROWS 行 RGB888 */
    uint32_t next_row;
} proof_source_t;

static esp_err_t proof_source_read(void *ctx, const uint8_t **ret_rows, size_t *ret_stride, int *ret_count)
{
    proof_source_t *s = ctx;
    const uint32_t width = s->frame.width;
    *ret_stride = (size_t)width * 3u;
    const uint32_t left = s->frame.height - s->next_row;
    const uint32_t count = left < PROOF_STRIP_ROWS ? left : PROOF_STRIP_ROWS;
    for (uint32_t r = 0; r < count; ++r) {
        film_viewfinder_row_to_rgb888(s->frame.uyvy + (size_t)(s->next_row + r) * s->frame.stride, width,
                                      s->rows + (size_t)r * *ret_stride);
    }
    s->next_row += count;
    *ret_rows = s->rows;
    *ret_count = (int)count;
    return ESP_OK;
}

/* ---------------------------------------------------------------- 一次冲洗的上下文 */

/* 各阶段的开始时刻（esp_timer 微秒，0 表示没经过），用于日志里的耗时分解 */
typedef struct {
    int64_t begin;
    int64_t proof_us;           /*!< 小样耗时 */
    int64_t wait_us;            /*!< 等上一张照片写盘的时间 */
    int64_t decode_us;          /*!< READ 阶段里花在 JPEG 解码上的部分 */
    int64_t stage[FILM_STAGE_COUNT];
} lab_timing_t;

/* 全尺寸冲洗的上下文（暗房任务栈上） */
typedef struct {
    film_lab_handle_t lab;
    job_t *job;
    const char *root;
    film_blob_t *raw;           /*!< 原片：解码器读、写盘任务写，各持一份引用 */
    jpeg_source_t *decoder;
    lab_timing_t *timing;
    bool report;                /*!< 是否给界面发进度（只预览时不发） */
    uint16_t sent;              /*!< 上次发出的进度（千分比） */
    int64_t sent_at;
} run_ctx_t;

static void on_progress(void *ctx, film_stage_t stage, float done)
{
    run_ctx_t *run = ctx;
    if (!run->timing->stage[stage]) {
        run->timing->stage[stage] = esp_timer_get_time();
    }
    if (!run->report) {
        return;
    }
    float total = 0.0f;
    for (int i = 0; i < (int)stage; ++i) {
        total += k_stage_weight[i];
    }
    total += k_stage_weight[stage] * (done < 0.0f ? 0.0f : (done > 1.0f ? 1.0f : done));
    /* 满值留给"取景已恢复"那一刻 */
    uint16_t permille = (uint16_t)(total * (float)FILM_PROGRESS_DONE);
    if (permille >= FILM_PROGRESS_DONE) {
        permille = FILM_PROGRESS_DONE - 1;
    }
    const int64_t now = esp_timer_get_time();
    if (permille < run->sent + PROGRESS_STEP && !(permille > run->sent && now - run->sent_at >= PROGRESS_MIN_GAP_US)) {
        return;
    }
    if (uxQueueSpacesAvailable(run->lab->config.events) <= PROGRESS_QUEUE_RESERVE) {
        return;
    }
    run->sent = permille;
    run->sent_at = now;
    const film_event_t ev = { .type = FILM_EVT_PROGRESS, .photo_id = run->job->shot.photo_id, .progress = permille };
    push_event(run->lab, &ev);
}

/** 相邻两个时刻之差（毫秒），任一端缺失时为 -1 */
static int span_ms(int64_t from, int64_t to)
{
    return from && to ? (int)((to - from) / 1000) : -1;
}

static void log_timing(const lab_timing_t *t, int64_t end)
{
    int64_t next[FILM_STAGE_COUNT];
    int64_t after = end;
    for (int i = FILM_STAGE_COUNT - 1; i >= 0; --i) {
        next[i] = after;
        if (t->stage[i]) {
            after = t->stage[i];
        }
    }
    ESP_LOGI(TAG, "timing ms: proof %d, wait %d | read %d (decode %d), filter %d, finish %d, previews %d, encode %d",
             (int)(t->proof_us / 1000), (int)(t->wait_us / 1000),
             span_ms(t->stage[FILM_STAGE_READ], next[FILM_STAGE_READ]), (int)(t->decode_us / 1000),
             span_ms(t->stage[FILM_STAGE_FILTER], next[FILM_STAGE_FILTER]),
             span_ms(t->stage[FILM_STAGE_FINISH], next[FILM_STAGE_FINISH]),
             span_ms(t->stage[FILM_STAGE_PREVIEWS], next[FILM_STAGE_PREVIEWS]),
             span_ms(t->stage[FILM_STAGE_ENCODE], next[FILM_STAGE_ENCODE]));
}

/* ---------------------------------------------------------------- 成片编码（硬件）与延后写盘 */

static esp_err_t encode(film_lab_handle_t lab, const film_darkroom_image_t *image, int quality, uint8_t *out,
                        size_t capacity, uint32_t *ret_bytes)
{
    const jpeg_encode_cfg_t config = {
        .width = image->width,
        .height = image->height,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = quality,
    };
    return jpeg_encoder_process(lab->encoder, &config, image->pixels, (uint32_t)image->size, out, (uint32_t)capacity,
                                ret_bytes);
}

/** 复制一份到恰好大小的缓冲交给写盘任务（编码器的输出缓冲按最坏情况预留，偏大） */
static film_blob_t *exact_copy(const void *data, size_t size)
{
    void *copy = heap_caps_malloc(size, FILE_CAPS);
    if (!copy) {
        log_psram("deferred write copy");
        return NULL;
    }
    memcpy(copy, data, size);
    return film_blob_wrap(copy, size);
}

/* film_studio 回调：编码成片，文件交给写盘任务（path 由写盘任务按编号重新拼出） */
static esp_err_t write_jpeg(void *ctx, film_darkroom_image_t *image, const char *path, uint32_t *ret_bytes)
{
    (void)path;
    run_ctx_t *run = ctx;
    /* 硬件编码器的 "RGB888" 在内存里是 B、G、R：原地交换 R 和 B */
    for (uint16_t y = 0; y < image->height; ++y) {
        uint8_t *p = image->pixels + (size_t)y * image->stride;
        for (uint16_t x = 0; x < image->width; ++x, p += 3) {
            const uint8_t r = p[0];
            p[0] = p[2];
            p[2] = r;
        }
    }
    const jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t capacity = 0;
    const size_t want = (size_t)image->width * image->height * JPEG_OUT_BYTES_NUM / JPEG_OUT_BYTES_DEN;
    uint8_t *out = jpeg_alloc_encoder_mem(want, &out_cfg, &capacity);
    if (!out) {
        log_psram("JPEG output buffer");
        return ESP_ERR_NO_MEM;
    }
    uint32_t bytes = 0;
    esp_err_t err = encode(run->lab, image, JPEG_QUALITY, out, capacity, &bytes);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 输出缓冲装不下（编码器已释放 2D-DMA 通道），降一档质量再来一次 */
        ESP_LOGW(TAG, "JPEG q%d overflowed %u KB, retrying at q%d", JPEG_QUALITY, (unsigned)(capacity / 1024),
                 JPEG_QUALITY_FALLBACK);
        err = encode(run->lab, image, JPEG_QUALITY_FALLBACK, out, capacity, &bytes);
    }
    film_blob_t *blob = err == ESP_OK ? exact_copy(out, bytes) : NULL;
    heap_caps_free(out);
    if (err == ESP_OK && !blob) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        const film_photo_t id_only = { .id = run->job->shot.photo_id };
        err = film_writer_file(run->lab->writer, run->root, &id_only, FILM_FILE_JPEG, blob);
    }
    *ret_bytes = bytes;
    return err;
}

/* film_studio 回调：.THM/.SCR 复制一份交给写盘任务 */
static esp_err_t write_preview(void *ctx, const film_photo_t *meta, film_file_kind_t kind, const uint16_t *pixels,
                               uint16_t width, uint16_t height)
{
    run_ctx_t *run = ctx;
    film_blob_t *blob = exact_copy(pixels, sizeof(uint16_t) * width * height);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }
    return film_writer_preview(run->lab->writer, run->root, meta, kind, blob, width, height);
}

/* 原片已全部解码进成片：放掉解码器和这边的原片引用，给 JPEG 输出缓冲腾地方 */
static void source_done(void *ctx)
{
    run_ctx_t *run = ctx;
    run->timing->decode_us = run->decoder->decode_us;
    jpeg_source_close(run->decoder);
    if (run->raw) {
        film_blob_unref(run->raw);
        run->raw = NULL;
    } else {
        heap_caps_free(run->job->jpeg);
        run->job->jpeg = NULL;
    }
}

static void *image_alloc(size_t bytes, void *ctx)
{
    (void)ctx;
    void *p = heap_caps_aligned_calloc(IMAGE_ALIGN, 1, (bytes + IMAGE_ALIGN - 1) & ~(size_t)(IMAGE_ALIGN - 1),
                                       IMAGE_CAPS);
    if (!p) {
        log_psram("print buffer");
    }
    return p;
}

static void image_free(void *ptr, void *ctx)
{
    (void)ctx;
    heap_caps_free(ptr);
}

/* ---------------------------------------------------------------- 屏幕图缓冲 */

static uint16_t *take_screen(film_lab_handle_t lab)
{
    if (xSemaphoreTake(lab->screen_free, pdMS_TO_TICKS(SCREEN_WAIT_MS)) != pdTRUE) {
        return NULL;
    }
    uint16_t *screen = NULL;
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    for (int i = 0; i < SCREEN_SLOTS && !screen; ++i) {
        if (!lab->screen_busy[i]) {
            lab->screen_busy[i] = true;
            screen = lab->screens[i];
        }
    }
    xSemaphoreGive(lab->lock);
    return screen;
}

void film_lab_release_screen(film_lab_handle_t lab, const uint16_t *screen)
{
    bool released = false;
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    for (int i = 0; i < SCREEN_SLOTS; ++i) {
        if (lab->screens[i] == screen && lab->screen_busy[i]) {
            lab->screen_busy[i] = false;
            released = true;
        }
    }
    xSemaphoreGive(lab->lock);
    if (released) {
        xSemaphoreGive(lab->screen_free);
    }
}

static void push_developed(film_lab_handle_t lab, const film_shot_t *shot, uint32_t id, uint16_t *screen,
                           const film_studio_result_t *result)
{
    const film_event_t developed = {
        .type = FILM_EVT_DEVELOPED, .photo_id = id, .screen = screen, .width = result->screen_w,
        .height = result->screen_h, .preview = shot->preview_only, .film = shot->film, .instant = shot->instant,
    };
    if (xQueueSend(lab->config.events, &developed, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping developed %lu", (unsigned long)id);
        film_lab_release_screen(lab, screen);
    }
}

/* ---------------------------------------------------------------- 小样 */

/**
 * 用快门前最后一帧取景（640×480）按同一套裁切、转向、胶卷与相纸冲一张屏幕尺寸的小样，
 * 几百毫秒就能让界面看到胶卷效果；全尺寸成片随后在同一个任务里冲。
 */
static esp_err_t develop_proof(film_lab_handle_t lab, job_t *job)
{
    uint16_t *screen = take_screen(lab);
    if (!screen) {
        return ESP_ERR_NO_MEM;
    }
    proof_source_t src = { .frame = job->proof };
    src.rows = heap_caps_malloc((size_t)PROOF_STRIP_ROWS * job->proof.width * 3u, FILE_CAPS);
    esp_err_t err = src.rows ? ESP_OK : ESP_ERR_NO_MEM;
    film_studio_result_t result = { 0 };
    film_shot_t shot = job->shot;
    shot.preview_only = true;
    if (err == ESP_OK) {
        const film_darkroom_source_t source = {
            .width = (uint16_t)job->proof.width, .height = (uint16_t)job->proof.height, .ctx = &src,
            .read = proof_source_read,
        };
        const film_studio_config_t studio = { .darkroom = lab->darkroom, .ret_stats = &job->proof_stats };
        err = film_studio_process(&studio, &shot, &source, screen, &result);
    }
    heap_caps_free(src.rows);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "proof: %s", esp_err_to_name(err));
        film_lab_release_screen(lab, screen);
        return err;
    }
    shot.preview_only = false;   /* 对界面来说这就是这次拍摄的成片 */
    push_developed(lab, &shot, shot.photo_id, screen, &result);
    return ESP_OK;
}

/* ---------------------------------------------------------------- 全尺寸冲洗 */

/** 记录原片校正量：用来观察拍照模式白平衡/曝光收敛得怎么样 */
static void log_balance(film_balance_t code)
{
    float gain[3];
    film_balance_gains(code, gain);
    ESP_LOGI(TAG, "balance 0x%05lx: gain R %.2f G %.2f B %.2f", (unsigned long)code, gain[0], gain[1], gain[2]);
}

/**
 * 冲一张（只预览时冲屏幕尺寸）。存盘的照片：原片一开始就交给写盘任务（与解码同时写），
 * JPEG/.THM/.SCR 在冲完后交过去，最后 commit；暗房失败时 commit 带错误码，由写盘任务清理。
 */
static esp_err_t run_job(film_lab_handle_t lab, job_t *job, uint16_t *screen, film_studio_result_t *result,
                         lab_timing_t *timing)
{
    const film_shot_t *shot = &job->shot;
    const char *root = get_root(lab);
    ESP_RETURN_ON_FALSE(root || shot->preview_only, FILM_ERR_STORAGE_FULL, TAG, "storage unavailable");

    esp_err_t err = ESP_OK;
    if (job->redevelop) {
        char from[FILM_PATH_MAX];
        film_library_path(root, shot->source_id, FILM_FILE_RAW, from, sizeof(from));
        err = read_file(from, &job->jpeg, &job->size);
    }
    run_ctx_t run = { .lab = lab, .job = job, .root = root, .timing = timing, .report = !shot->preview_only };
    const film_photo_t id_only = { .id = shot->photo_id };
    uint8_t *const jpeg = job->jpeg;   /* 交给 blob 之后解码器仍从这里读 */
    const size_t size = job->size;
    /* 新照片都有自己的一份原片：拍摄时就是快门帧，重新冲洗另存时复制源照片的 */
    if (err == ESP_OK && !shot->preview_only) {
        run.raw = film_blob_wrap(job->jpeg, job->size);
        job->jpeg = NULL;   /* 归 blob 了 */
        err = run.raw ? ESP_OK : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = film_writer_file(lab->writer, root, &id_only, FILM_FILE_RAW, film_blob_ref(run.raw));
        }
    }
    jpeg_source_t decoder;
    film_darkroom_source_t source;
    if (err == ESP_OK) {
        err = jpeg_source_open(&decoder, jpeg, size, &source);
    }
    if (err == ESP_OK) {
        run.decoder = &decoder;
        /* 拍照模式的白平衡与曝光收敛不完全：有小样时把原片对齐到小样（重新冲洗沿用存档的校正） */
        const film_studio_config_t studio = {
            .darkroom = lab->darkroom, .root = root, .write_jpeg = write_jpeg, .write_preview = write_preview,
            .source_done = source_done, .ctx = &run, .progress = on_progress, .progress_ctx = &run,
            .match = job->proof_stats.valid ? &job->proof_stats : NULL,
        };
        err = film_studio_process(&studio, shot, &source, screen, result);
        if (err == ESP_OK) {
            log_balance(result->balance);
            if (studio.match) {
                film_still_learn_awb(&job->awb, result->balance);
            }
        }
        jpeg_source_close(&decoder);   /* 冲洗失败时 source_done 没被调用；关闭可重复 */
    }
    film_blob_unref(run.raw);
    if (!shot->preview_only && root) {
        const esp_err_t commit = film_writer_commit(lab->writer, root, err == ESP_OK ? &result->meta : &id_only, err);
        err = err == ESP_OK ? commit : err;
    }
    return err;
}

static void lab_task(void *arg)
{
    film_lab_handle_t lab = arg;
    bool warmed = false;
    for (;;) {
        job_t job;
        if (xQueueReceive(lab->jobs, &job, warmed ? portMAX_DELAY : pdMS_TO_TICKS(WARM_UP_IDLE_MS)) != pdTRUE) {
            if (!warmed) {
                film_storage_warm_up();
                warmed = true;
            }
            continue;
        }
        warmed = true;   /* 第一张照片自己会触发那次遍历 */
        const film_shot_t *shot = &job.shot;
        const uint32_t event_id = shot->preview_only ? shot->source_id : shot->photo_id;
        /* 先让取景停下并交出帧缓冲，再开始分配冲洗用的大块内存 */
        lab->config.on_busy(lab->config.ctx, true);
        lab_timing_t timing = { .begin = esp_timer_get_time() };

        bool proof_sent = false;
        if (job.proof.uyvy) {
            proof_sent = develop_proof(lab, &job) == ESP_OK;
            heap_caps_free((void *)job.proof.uyvy);
            job.proof.uyvy = NULL;
            timing.proof_us = esp_timer_get_time() - timing.begin;
        }
        /* 两张照片的大块缓冲同时放不下：上一张还在写盘就先等它写完 */
        if (!shot->preview_only) {
            const int64_t t0 = esp_timer_get_time();
            (void)film_writer_wait_idle(lab->writer, portMAX_DELAY);
            timing.wait_us = esp_timer_get_time() - t0;
        }

        const size_t free_at_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        (void)heap_caps_monitor_local_minimum_free_size_start();
        uint16_t *screen = take_screen(lab);
        film_studio_result_t result = { 0 };
        esp_err_t err = screen ? ESP_OK : ESP_ERR_NO_MEM;
        if (err == ESP_OK) {
            err = run_job(lab, &job, screen, &result, &timing);
        }
        log_timing(&timing, esp_timer_get_time());
        ESP_LOGI(TAG, "job PSRAM peak %u KB (free at start %u KB)",
                 (unsigned)((free_at_start - heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)) / 1024),
                 (unsigned)(free_at_start / 1024));
        (void)heap_caps_monitor_local_minimum_free_size_stop();
        heap_caps_free(job.jpeg);
        lab->config.on_busy(lab->config.ctx, false);
        ESP_LOGI(TAG, "%s %lu film=%d instant=%d -> %s in %lld ms, PSRAM free %u KB",
                 job.redevelop ? (shot->preview_only ? "preview" : "redevelop") : "shoot", (unsigned long)event_id,
                 (int)shot->film, (int)shot->instant, esp_err_to_name(err),
                 (long long)((esp_timer_get_time() - timing.begin) / 1000),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

        if (err == ESP_OK) {
            /* 有小样时界面已经拿到屏幕图了；成片的屏幕图已复制给写盘任务 */
            if (proof_sent) {
                film_lab_release_screen(lab, screen);
            } else {
                push_developed(lab, shot, event_id, screen, &result);
            }
            if (!shot->preview_only) {
                const film_event_t done = {
                    .type = FILM_EVT_PROGRESS, .photo_id = event_id, .progress = FILM_PROGRESS_DONE,
                };
                push_event(lab, &done);
            }
        } else {
            if (screen) {
                film_lab_release_screen(lab, screen);
            }
            const film_event_t failed = { .type = FILM_EVT_SHOT_FAILED, .err = err, .photo_id = event_id };
            push_event(lab, &failed);
        }
        set_busy(lab, false);
    }
}

/* ---------------------------------------------------------------- 公共接口 */

void film_lab_set_root(film_lab_handle_t lab, const char *root)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->root = root;
    xSemaphoreGive(lab->lock);
}

bool film_lab_busy(film_lab_handle_t lab)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    const bool busy = lab->busy;
    xSemaphoreGive(lab->lock);
    return busy;
}

esp_err_t film_lab_reserve(film_lab_handle_t lab)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    if (lab->busy) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!lab->root) {
        err = FILM_ERR_STORAGE_FULL;
    } else {
        lab->busy = true;
    }
    xSemaphoreGive(lab->lock);
    return err;
}

void film_lab_cancel_reserve(film_lab_handle_t lab)
{
    set_busy(lab, false);
}

void film_lab_deliver_still(film_lab_handle_t lab, const film_shot_t *shot, const film_still_t *still,
                            const film_viewfinder_source_t *proof, esp_err_t err)
{
    uint8_t *jpeg = still->jpeg;
    uint8_t *proof_px = proof ? (uint8_t *)proof->uyvy : NULL;
    if (err != ESP_OK || !jpeg) {
        heap_caps_free(jpeg);
        heap_caps_free(proof_px);
        const film_event_t failed = {
            .type = FILM_EVT_SHOT_FAILED, .err = err != ESP_OK ? err : ESP_FAIL, .photo_id = shot->photo_id,
        };
        push_event(lab, &failed);
        lab->config.on_busy(lab->config.ctx, false);   /* 没有冲洗可做：放开拍照时暂停的取景 */
        set_busy(lab, false);
        return;
    }
    const film_event_t done = { .type = FILM_EVT_SHUTTER_DONE, .photo_id = shot->photo_id };
    push_event(lab, &done);
    job_t job = { .shot = *shot, .redevelop = false, .jpeg = jpeg, .size = still->size, .awb = still->awb };
    if (proof_px) {
        job.proof = *proof;
    }
    if (xQueueSend(lab->jobs, &job, 0) != pdTRUE) {
        heap_caps_free(jpeg);
        heap_caps_free(proof_px);
        const film_event_t failed = { .type = FILM_EVT_SHOT_FAILED, .err = ESP_ERR_NO_MEM, .photo_id = shot->photo_id };
        push_event(lab, &failed);
        lab->config.on_busy(lab->config.ctx, false);
        set_busy(lab, false);
    }
}

esp_err_t film_lab_redevelop(film_lab_handle_t lab, const film_shot_t *shot)
{
    ESP_RETURN_ON_ERROR(film_lab_reserve(lab), TAG, "redevelop busy");
    const job_t job = { .shot = *shot, .redevelop = true };
    if (xQueueSend(lab->jobs, &job, 0) != pdTRUE) {
        set_busy(lab, false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- 创建 */

typedef struct {
    film_lab_handle_t lab;
    SemaphoreHandle_t done;
    esp_err_t err;
} boot_ctx_t;

/* 滤镜实例要在暗房任务所在的核上创建（工作任务会落在另一个核） */
static esp_err_t lab_init_in_task(film_lab_handle_t lab)
{
    const film_filter_config_t filter = {
        .max_width = FILTER_MAX_SIDE, .max_height = FILTER_MAX_SIDE, .parallel = true,
    };
    ESP_RETURN_ON_ERROR(film_filter_create(&filter, &lab->filter), TAG, "filter");
    const film_darkroom_config_t darkroom = {
        .filter = lab->filter, .alloc = image_alloc, .free = image_free, .alloc_ctx = lab,
    };
    ESP_RETURN_ON_ERROR(film_darkroom_create(&darkroom, &lab->darkroom), TAG, "darkroom");
    const jpeg_encode_engine_cfg_t encoder = { .timeout_ms = JPEG_TIMEOUT_MS };
    return jpeg_new_encoder_engine(&encoder, &lab->encoder);
}

static void lab_task_entry(void *arg)
{
    boot_ctx_t *boot = arg;
    film_lab_handle_t lab = boot->lab;
    const esp_err_t err = lab_init_in_task(lab);
    boot->err = err;
    xSemaphoreGive(boot->done);   /* 之后 boot 随创建者的栈失效 */
    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    lab_task(lab);
}

static void lab_free(film_lab_handle_t lab)
{
    if (lab->writer) {
        film_writer_delete(lab->writer);
    }
    if (lab->encoder) {
        (void)jpeg_del_encoder_engine(lab->encoder);
    }
    if (lab->darkroom) {
        film_darkroom_delete(lab->darkroom);
    }
    if (lab->filter) {
        (void)film_filter_delete(lab->filter);
    }
    for (int i = 0; i < SCREEN_SLOTS; ++i) {
        heap_caps_free(lab->screens[i]);
    }
    if (lab->screen_free) {
        vSemaphoreDelete(lab->screen_free);
    }
    if (lab->jobs) {
        vQueueDelete(lab->jobs);
    }
    if (lab->lock) {
        vSemaphoreDelete(lab->lock);
    }
    free(lab);
}

esp_err_t film_lab_start(const film_lab_config_t *config, film_lab_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && config->events && config->on_busy && ret_handle, ESP_ERR_INVALID_ARG, TAG,
                        "bad args");
    film_lab_handle_t lab = calloc(1, sizeof(*lab));
    ESP_RETURN_ON_FALSE(lab, ESP_ERR_NO_MEM, TAG, "lab");
    lab->config = *config;
    lab->lock = xSemaphoreCreateMutex();
    lab->jobs = xQueueCreate(JOB_QUEUE_LEN, sizeof(job_t));
    lab->screen_free = xSemaphoreCreateCounting(SCREEN_SLOTS, SCREEN_SLOTS);
    bool ok = lab->lock && lab->jobs && lab->screen_free;
    for (int i = 0; ok && i < SCREEN_SLOTS; ++i) {
        lab->screens[i] = heap_caps_calloc(FILM_STUDIO_SCREEN_PIXELS, sizeof(uint16_t), FILE_CAPS);
        ok = lab->screens[i] != NULL;
    }
    esp_err_t err = ok ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        /* 写盘任务是单例，随暗房一起常驻（暗房启动失败时任务尚未创建） */
        const film_writer_config_t writer = { .priority = config->writer_priority, .events = config->events };
        err = film_writer_start(&writer, &lab->writer);
    }
    boot_ctx_t boot = { .lab = lab };
    if (err == ESP_OK) {
        boot.done = xSemaphoreCreateBinary();
        err = boot.done ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = xTaskCreatePinnedToCore(lab_task_entry, "film_lab", LAB_TASK_STACK, &boot, config->priority,
                                      &lab->task, config->core) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        xSemaphoreTake(boot.done, portMAX_DELAY);
        err = boot.err;
    }
    if (boot.done) {
        vSemaphoreDelete(boot.done);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        lab_free(lab);
        return err;
    }
    *ret_handle = lab;
    return ESP_OK;
}

#include "film_lab.h"

#include <stdatomic.h>
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
#include "film_meta.h"
#include "film_perf.h"
#include "film_storage.h"
#include "film_studio.h"
#include "film_writer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "film_lab";

#define LAB_TASK_STACK          8192
#define JOB_QUEUE_LEN           4       /*!< 小样任务（每张新照片一个）与重新冲洗任务 */
#define POLL_MS                 20      /*!< 有一张在等冲洗时，暗房多久检查一次"小样出了吗、原片落盘了吗" */
#define RAW_WRITE_WAIT_MS       10000   /*!< 等原片落盘的上限（落盘失败时 RAW 不在，冲洗会如实失败） */
#define SCREEN_SLOTS            2       /*!< 一张界面在显示、一张给下一次冲洗 */
#define SCREEN_WAIT_MS          5000
#define FILTER_MAX_SIDE         FILM_DARKROOM_MAX_EDGE  /*!< film_darkroom 要求滤镜实例至少这么大 */
#define JPEG_QUALITY            92
#define JPEG_QUALITY_FALLBACK   80      /*!< 极少数细节很多的照片超出输出缓冲时，降一档重编 */
#define JPEG_TIMEOUT_MS         2000
#define JPEG_OUT_BYTES_NUM      2       /*!< 输出缓冲按每像素 0.4 字节预留（q92 实际约 0.3）；超出时降质量重编 */
#define JPEG_OUT_BYTES_DEN      5
#define WARM_UP_DELAY_MS        2000    /*!< 开机后这么久再预热存储：让界面先读完相册索引（预热期间文件系统整体被锁住） */
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
    bool proof_only;            /*!< 新拍照片的小样任务；全尺寸冲洗紧接着由 pending 触发 */
    uint32_t raw_id;            /*!< 全尺寸冲洗读哪张照片的 RAW 文件（0 表示没有，只做小样） */
    bool copy_raw;              /*!< 另存为新照片：复制一份 RAW（拼 EXIF）给新编号；新拍照片的 RAW 已在盘上，不复制 */
    uint8_t *jpeg;              /*!< 读进内存的原片（任务负责释放）；小样任务为 NULL */
    size_t size;
    film_viewfinder_source_t proof; /*!< 快门前最后一帧取景（uyvy 归任务所有，可能为 NULL） */
    film_tone_stats_t proof_stats;  /*!< 小样原片的通道均值：成片原片对齐到它（见 film_balance.h） */
    film_still_awb_t awb;           /*!< 拍摄所用的白平衡增益（只对新拍的照片有意义） */
    bool proof_sent;                /*!< 界面已经拿到这张的小样：成片冲完不再发 DEVELOPED */
} job_t;

/** 刚拍下的这一张：RAW 已交给写盘任务，小样一出就接着冲成片（lock 保护） */
typedef struct {
    film_shot_t shot;
    film_tone_stats_t proof_stats;
    film_still_awb_t awb;
    bool proof_sent;
    bool ready;                 /*!< 小样阶段结束（出了小样，或没有小样/小样失败）：可以开始冲成片 */
} pending_t;

struct film_lab_t {
    film_lab_config_t config;
    TaskHandle_t task;
    QueueHandle_t jobs;
    film_writer_handle_t writer;

    /* lock 保护下面这一组 */
    SemaphoreHandle_t lock;
    bool capturing;             /*!< 已预约快门，原片还没交到（相机在取景→拍照切换中） */
    bool redeveloping;          /*!< 有重新冲洗在排队/处理（界面在等结果，同一时刻只允许一个） */
    bool developing;            /*!< 暗房正在冲刚拍下的这一张 */
    bool has_pending;           /*!< 有一张刚拍下、还没冲完（拍—冲是串行的：冲完才能拍下一张） */
    pending_t pending;
    bool warmed;                /*!< 存储预热完成：在此之前不接快门（预热期间任何文件操作都会被卡住） */
    const char *root;
    bool screen_busy[SCREEN_SLOTS];

    SemaphoreHandle_t screen_free;  /*!< 空闲屏幕图缓冲计数 */
    uint16_t *screens[SCREEN_SLOTS];

    /* 只在暗房任务里使用 */
    film_filter_handle_t filter;
    film_darkroom_handle_t darkroom;
    jpeg_encoder_handle_t encoder;
    SemaphoreHandle_t encoder_lock; /*!< 硬件编码器同一时刻只做一件事：成片编码（暗房任务）与无 JPEG 传感器的原片编码（相机任务）共用 */
};

/* ---------------------------------------------------------------- 小工具 */

static void push_event(film_lab_handle_t lab, const film_event_t *ev)
{
    if (xQueueSend(lab->config.events, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping type %d", ev->type);
    }
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
    taskYIELD();   /* 条带边界让一下：界面与写盘任务不会被整幅解码饿住 */
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
    film_blob_t *raw;           /*!< 写盘用的原片（带 EXIF）：这边与写盘任务各持一份引用 */
    bool source_in_raw;         /*!< 为真：解码器读的就是 raw 的内存（退化为零拷贝时），解码完要放掉 raw 才释放原片 */
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
    FILM_PERF_LOG("lab",
                  "stages proof=%d wait_writer=%d read=%d decode=%d filter=%d finish=%d previews=%d encode=%d ms",
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
        /* 存盘成片是 YUV420 存放（见 film_yuv.h，与硬件格式一致），直接编码，不需要再转换 */
        .src_type = image->format == FILM_IMAGE_YUV420 ? JPEG_ENCODE_IN_FORMAT_YUV420 : JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = quality,
    };
    xSemaphoreTake(lab->encoder_lock, portMAX_DELAY);
    const esp_err_t err = jpeg_encoder_process(lab->encoder, &config, image->pixels, (uint32_t)image->size, out,
                                               (uint32_t)capacity, ret_bytes);
    xSemaphoreGive(lab->encoder_lock);
    return err;
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
    /* 硬件编码器的 "RGB888" 在内存里是 B、G、R：原地交换 R 和 B（YUV420 不涉及） */
    for (uint16_t y = 0; image->format == FILM_IMAGE_RGB888 && y < image->height; ++y) {
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
    film_perf_mem("lab encode (input image + output buffer live)");
    uint32_t bytes = 0;
    const int64_t encode_t0 = FILM_PERF_NOW();
    int quality = JPEG_QUALITY;
    esp_err_t err = encode(run->lab, image, JPEG_QUALITY, out, capacity, &bytes);
    if (err == ESP_ERR_INVALID_STATE) {
        quality = JPEG_QUALITY_FALLBACK;
        /* 输出缓冲装不下（编码器已释放 2D-DMA 通道），降一档质量再来一次 */
        ESP_LOGW(TAG, "JPEG q%d overflowed %u KB, retrying at q%d", JPEG_QUALITY, (unsigned)(capacity / 1024),
                 JPEG_QUALITY_FALLBACK);
        err = encode(run->lab, image, JPEG_QUALITY_FALLBACK, out, capacity, &bytes);
    }
    if (err != ESP_OK) {
        /* 成片编码失败：记下尺寸、格式与内存，设备上才能看出是哪一步没内存 */
        ESP_LOGE(TAG, "encode %ux%u fmt=%d q%d -> %s (out buffer %u KB)", (unsigned)image->width,
                 (unsigned)image->height, (int)image->format, quality, esp_err_to_name(err),
                 (unsigned)(capacity / 1024));
        log_psram("encode failed");
    }
    FILM_PERF_LOG("lab", "jpeg encode %ux%u q%d: %u KB in %lld ms (out buffer %u KB) -> %s", (unsigned)image->width,
                  (unsigned)image->height, quality, (unsigned)(bytes / 1024),
                  (long long)((FILM_PERF_NOW() - encode_t0) / 1000), (unsigned)(capacity / 1024),
                  esp_err_to_name(err));
    /*
     * 成片像素在编码后不再需要（屏幕图与缩略图在编码前已取好）：先放掉再拼 EXIF。
     * 拍立得的整张相纸 YUV420 约 5.4 MB，不放的话下面复制 JPEG 时 PSRAM 已经见底
     * （设备日志：EXIF copy: PSRAM free 0 KB，每张拍立得都因此冲洗失败）。release 可重复调用。
     */
    film_darkroom_release(run->lab->darkroom, image);
    /* 成片带 EXIF：拼接时复制一份恰好大小的缓冲（编码器输出缓冲按最坏情况预留，偏大） */
    film_blob_t *blob = err == ESP_OK ? film_meta_make_blob(&run->job->shot, FILM_META_JPG, out, bytes) : NULL;
    heap_caps_free(out);
    if (err == ESP_OK && !blob) {
        ESP_LOGE(TAG, "JPEG %u KB: no memory for the EXIF copy", (unsigned)(bytes / 1024));
        log_psram("EXIF copy");
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
    if (!run->source_in_raw) {
        heap_caps_free(run->job->jpeg);
        run->job->jpeg = NULL;
    }
    film_blob_unref(run->raw);   /* 退化路径下这一步释放解码器读的内存；正常路径只是放掉这边的引用 */
    run->raw = NULL;
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
    if (job->raw_id) {
        char from[FILM_PATH_MAX];
        film_library_path(root, job->raw_id, FILM_FILE_RAW, from, sizeof(from));
        err = read_file(from, &job->jpeg, &job->size);
    }
    run_ctx_t run = { .lab = lab, .job = job, .root = root, .timing = timing, .report = !shot->preview_only };
    const film_photo_t id_only = { .id = shot->photo_id };
    /*
     * 每张新照片都有自己的一份原片：拍摄时已在 deliver_still 里落盘（这里不再写），
     * 重新冲洗另存为新照片时才复制源照片的一份。
     * 原片文件带 EXIF（含 Orientation），所以写盘用的是"复制 + 拼 EXIF"的一份；解码器仍读原来那份
     * （不依赖解码库对 APP1 段的处理），解码完在 source_done 里释放。
     * 复制失败（内存不足）时退回零拷贝：原片直接交给写盘任务，不带 EXIF，照片不丢。
     */
    uint8_t *const jpeg = job->jpeg;
    const size_t size = job->size;
    if (err == ESP_OK && !shot->preview_only && job->copy_raw) {
        run.raw = film_meta_make_blob(shot, FILM_META_RAW, jpeg, size);
        if (!run.raw) {
            ESP_LOGW(TAG, "RAW without EXIF (no memory for the copy)");
            run.raw = film_blob_wrap(jpeg, size);
            if (run.raw) {
                job->jpeg = NULL;           /* 归 blob 了；解码器仍从同一块内存读 */
                run.source_in_raw = true;
            }
        }
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
    /* 成败都 commit：失败时由写盘任务清掉这张照片的全部文件（含 RAW），界面收到 SHOT_FAILED */
    if (!shot->preview_only && root) {
        const film_photo_t *meta = err == ESP_OK ? &result->meta : &id_only;
        const esp_err_t commit = film_writer_commit(lab->writer, root, meta, err);
        err = err == ESP_OK ? commit : err;
    }
    return err;
}

/* ---------------------------------------------------------------- 刚拍下的这一张 */

static bool pending_exists(film_lab_handle_t lab)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    const bool has = lab->has_pending;
    xSemaphoreGive(lab->lock);
    return has;
}

static void pending_set(film_lab_handle_t lab, const pending_t *item)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->pending = *item;
    lab->has_pending = true;
    xSemaphoreGive(lab->lock);
}

static void pending_clear(film_lab_handle_t lab)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->has_pending = false;
    xSemaphoreGive(lab->lock);
}

/** 小样阶段结束：记下对齐用的统计与"界面已拿到小样"，并放行成片冲洗 */
static void pending_set_proof(film_lab_handle_t lab, uint32_t photo_id, const film_tone_stats_t *stats, bool sent)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    if (lab->has_pending && lab->pending.shot.photo_id == photo_id) {
        lab->pending.proof_stats = *stats;
        lab->pending.proof_sent = sent;
        lab->pending.ready = true;
    }
    xSemaphoreGive(lab->lock);
}

/* ---------------------------------------------------------------- 暗房任务 */

/**
 * 全尺寸冲洗一张（刚拍下的新照片、界面发起的重新冲洗共用）：
 * 停取景 → run_job → 恢复取景 → 发结果事件（成功：DEVELOPED/DONE，失败：SHOT_FAILED）。
 * proof_sent：界面已经拿到这张的小样（成片冲完只释放屏幕图，不再发 DEVELOPED）。
 */
static esp_err_t develop_job(film_lab_handle_t lab, job_t *job)
{
    const film_shot_t *shot = &job->shot;
    const uint32_t event_id = shot->preview_only ? shot->source_id : shot->photo_id;
    /* 先让取景停下并交出帧缓冲，再开始分配冲洗用的大块内存 */
    lab->config.on_busy(lab->config.ctx, true);
    lab_timing_t timing = { .begin = esp_timer_get_time() };
    film_perf_mark(FILM_PERF_LAB_START);
    film_perf_mem("lab start (viewfinder stopped)");
    film_perf_cpu_reset();

    const size_t free_at_start = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    (void)heap_caps_monitor_local_minimum_free_size_start();
    uint16_t *screen = take_screen(lab);
    film_studio_result_t result = { 0 };
    esp_err_t err = screen ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        err = run_job(lab, job, screen, &result, &timing);
    }
    log_timing(&timing, esp_timer_get_time());
    ESP_LOGI(TAG, "job PSRAM peak %u KB (free at start %u KB)",
             (unsigned)((free_at_start - heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)) / 1024),
             (unsigned)(free_at_start / 1024));
    (void)heap_caps_monitor_local_minimum_free_size_stop();
    heap_caps_free(job->jpeg);
    job->jpeg = NULL;
    film_perf_mark(FILM_PERF_FULL_DONE);
    film_perf_mem("lab end");
    film_perf_cpu_report("develop");
    lab->config.on_busy(lab->config.ctx, false);
    const char *what = shot->preview_only ? "preview" : (job->copy_raw ? "redevelop" : "develop");
    ESP_LOGI(TAG, "%s %lu film=%d instant=%d -> %s in %lld ms, PSRAM free %u KB", what, (unsigned long)event_id,
             (int)shot->film, (int)shot->instant, esp_err_to_name(err),
             (long long)((esp_timer_get_time() - timing.begin) / 1000),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    if (err == ESP_OK) {
        /* 有小样时界面已经拿到屏幕图了；成片的屏幕图已复制给写盘任务 */
        if (job->proof_sent) {
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
    return err;
}

/** 新拍照片的小样：取景帧冲成屏幕图让界面马上看到胶卷效果；不停取景。随后把对齐统计记下并放行成片冲洗 */
static void run_proof_job(film_lab_handle_t lab, job_t *job)
{
    bool sent = false;
    if (job->proof.uyvy) {
        const int64_t t0 = esp_timer_get_time();
        sent = develop_proof(lab, job) == ESP_OK;
        heap_caps_free((void *)job->proof.uyvy);
        job->proof.uyvy = NULL;
        ESP_LOGI(TAG, "proof %lu: %s in %lld ms", (unsigned long)job->shot.photo_id, sent ? "sent" : "failed",
                 (long long)((esp_timer_get_time() - t0) / 1000));
        film_perf_mark(FILM_PERF_PROOF_DONE);
    }
    pending_set_proof(lab, job->shot.photo_id, &job->proof_stats, sent);
}

/** 小样出了、原片落盘了就开冲刚拍下的这一张；还没到时什么都不做（下一轮轮询再看） */
static void develop_pending(film_lab_handle_t lab)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    const bool go = lab->has_pending && lab->pending.ready;
    const pending_t item = go ? lab->pending : (pending_t){ 0 };
    xSemaphoreGive(lab->lock);
    if (!go) {
        return;
    }
    /* 原片要先写完：冲洗从盘上的 RAW 读（写盘失败时 RAW 不在，冲洗会如实失败并通知界面） */
    (void)film_writer_wait_idle(lab->writer, RAW_WRITE_WAIT_MS);

    job_t job = {
        .shot = item.shot, .raw_id = item.shot.photo_id,
        .proof_stats = item.proof_stats, .awb = item.awb, .proof_sent = item.proof_sent,
    };
    if (lab->config.can_develop && !lab->config.can_develop(lab->config.ctx)) {
        /* 电量太低（快门前已拦过，这里是拍摄途中掉下来的）：不开冲，如实报失败，释放取景 */
        const film_event_t failed = { .type = FILM_EVT_SHOT_FAILED, .err = FILM_ERR_LOW_BATTERY, .photo_id = job.shot.photo_id };
        push_event(lab, &failed);
        (void)film_writer_commit(lab->writer, get_root(lab), &(film_photo_t){ .id = job.shot.photo_id }, failed.err);
        lab->config.on_busy(lab->config.ctx, false);
        pending_clear(lab);
        return;
    }
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->developing = true;
    xSemaphoreGive(lab->lock);

    (void)develop_job(lab, &job);

    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->developing = false;
    xSemaphoreGive(lab->lock);
    pending_clear(lab);   /* 成败都清：失败已经通知界面，快门解锁 */
}

static void lab_task(void *arg)
{
    film_lab_handle_t lab = arg;
    /*
     * 存储预热：LittleFS 第一次分配块要整盘扫描（耗时随照片数增长），期间整个文件系统被锁住。
     * 与其让第一张照片撞上它，不如开机后尽早做完；做完之前 film_lab_reserve 不接快门。
     */
    vTaskDelay(pdMS_TO_TICKS(WARM_UP_DELAY_MS));
    film_storage_warm_up();
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->warmed = true;
    xSemaphoreGive(lab->lock);

    for (;;) {
        job_t job;
        /* 有一张在等：短轮询；没有就睡到有任务 */
        const TickType_t wait = pending_exists(lab) ? pdMS_TO_TICKS(POLL_MS) : portMAX_DELAY;
        if (xQueueReceive(lab->jobs, &job, wait) == pdTRUE) {
            if (job.proof_only) {
                run_proof_job(lab, &job);
            } else {
                /* 界面发起的重新冲洗：用户在等结果 */
                (void)develop_job(lab, &job);
                xSemaphoreTake(lab->lock, portMAX_DELAY);
                lab->redeveloping = false;
                xSemaphoreGive(lab->lock);
            }
            continue;
        }
        develop_pending(lab);
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
    const bool busy = lab->capturing || lab->redeveloping || lab->developing || lab->has_pending;
    xSemaphoreGive(lab->lock);
    return busy;
}

bool film_lab_working(film_lab_handle_t lab)
{
    return film_lab_busy(lab) || !film_writer_wait_idle(lab->writer, 0);
}

esp_err_t film_lab_reserve(film_lab_handle_t lab)
{
    esp_err_t err = ESP_OK;
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    if (!lab->warmed) {
        err = FILM_ERR_STORAGE_PREPARING;   /* 开机预热还没做完 */
    } else if (lab->capturing || lab->redeveloping || lab->developing || lab->has_pending) {
        err = ESP_ERR_INVALID_STATE;        /* 拍—冲串行：上一张冲完才能拍下一张 */
    } else if (!lab->root) {
        err = FILM_ERR_STORAGE_FULL;
    } else {
        lab->capturing = true;
    }
    xSemaphoreGive(lab->lock);
    return err;
}

void film_lab_cancel_reserve(film_lab_handle_t lab)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->capturing = false;
    xSemaphoreGive(lab->lock);
}

/**
 * 没有 JPEG 的传感器（SC101IOT）：把快门帧（4:3 窗口的 UYVY，紧凑或带 stride）用硬件编码器编成 JPEG 原片。
 * 先逐行转 RGB888 并交换 R/B（编码器的 "RGB888" 在内存里是 B、G、R），再整块编码；
 * 宽高向下取 16 的倍数（编码器要求），多出的边缘舍去。成功时 *ret_jpeg 是 PSRAM 里恰好大小的副本。
 * 在相机任务里调用（快门后、SHUTTER_DONE 之前），编码器用 encoder_lock 与暗房互斥。
 */
static esp_err_t encode_frame_jpeg(film_lab_handle_t lab, const film_viewfinder_source_t *src, uint8_t **ret_jpeg,
                                   size_t *ret_size)
{
    const uint32_t width = src->width & ~15u;
    const uint32_t height = src->height & ~15u;
    ESP_RETURN_ON_FALSE(src->uyvy && width && height, ESP_ERR_INVALID_SIZE, TAG, "frame %ux%u", (unsigned)src->width,
                        (unsigned)src->height);
    const size_t stride = src->stride ? src->stride : (size_t)src->width * 2u;
    const jpeg_encode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    const jpeg_encode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t in_size = 0, capacity = 0;
    uint8_t *rgb = jpeg_alloc_encoder_mem((size_t)width * height * 3u, &in_cfg, &in_size);
    uint8_t *out = jpeg_alloc_encoder_mem((size_t)width * height * JPEG_OUT_BYTES_NUM / JPEG_OUT_BYTES_DEN, &out_cfg,
                                          &capacity);
    esp_err_t err = rgb && out ? ESP_OK : ESP_ERR_NO_MEM;
    for (uint32_t y = 0; err == ESP_OK && y < height; ++y) {
        uint8_t *row = rgb + (size_t)y * width * 3u;
        film_viewfinder_row_to_rgb888(src->uyvy + (size_t)y * stride, width, row);
        for (uint32_t x = 0; x < width; ++x, row += 3) {
            const uint8_t r = row[0];
            row[0] = row[2];
            row[2] = r;
        }
    }
    uint32_t bytes = 0;
    if (err == ESP_OK) {
        const film_darkroom_image_t image = {
            .pixels = rgb, .size = (size_t)width * height * 3u, .width = (uint16_t)width, .height = (uint16_t)height,
            .stride = (size_t)width * 3u, .format = FILM_IMAGE_RGB888,
        };
        err = encode(lab, &image, JPEG_QUALITY, out, capacity, &bytes);
        if (err == ESP_ERR_INVALID_STATE) {
            err = encode(lab, &image, JPEG_QUALITY_FALLBACK, out, capacity, &bytes);
        }
    }
    uint8_t *copy = err == ESP_OK ? heap_caps_malloc(bytes, FILE_CAPS) : NULL;
    if (copy) {
        memcpy(copy, out, bytes);
        *ret_jpeg = copy;
        *ret_size = bytes;
    } else if (err == ESP_OK) {
        err = ESP_ERR_NO_MEM;
    }
    heap_caps_free(rgb);
    heap_caps_free(out);
    ESP_LOGI(TAG, "frame %ux%u -> JPEG %u KB: %s", (unsigned)width, (unsigned)height, (unsigned)(bytes / 1024),
             esp_err_to_name(err));
    return err;
}

/** 拍摄失败：放掉缓冲、报失败、撤销预约，并放开取景（相机在受理快门时已暂停取景，等着冲洗来恢复） */
static void fail_capture(film_lab_handle_t lab, uint32_t photo_id, esp_err_t err, void *jpeg, void *proof_px)
{
    heap_caps_free(jpeg);
    heap_caps_free(proof_px);
    const film_event_t failed = { .type = FILM_EVT_SHOT_FAILED, .err = err, .photo_id = photo_id };
    push_event(lab, &failed);
    film_lab_cancel_reserve(lab);
    lab->config.on_busy(lab->config.ctx, false);
}

void film_lab_deliver_still(film_lab_handle_t lab, const film_shot_t *shot, const film_still_t *still,
                            const film_viewfinder_source_t *proof, esp_err_t err)
{
    uint8_t *jpeg = still->jpeg;
    size_t jpeg_size = still->size;
    uint8_t *proof_px = proof ? (uint8_t *)proof->uyvy : NULL;
    const char *root = get_root(lab);
    if (err == ESP_OK && !jpeg && still->from_frame && proof && proof->uyvy) {
        err = encode_frame_jpeg(lab, proof, &jpeg, &jpeg_size);   /* 传感器没有 JPEG：用快门帧编一份原片 */
    }
    if (err != ESP_OK || !jpeg) {
        fail_capture(lab, shot->photo_id, err != ESP_OK ? err : ESP_FAIL, jpeg, proof_px);
        return;
    }
    if (!root) {
        fail_capture(lab, shot->photo_id, FILM_ERR_STORAGE_FULL, jpeg, proof_px);
        return;
    }
    /*
     * 原片先落盘：带 EXIF 的一份交给写盘任务，内存里只剩这一份拷贝，
     * 写完即释放；之后全尺寸冲洗从文件读回。拼 EXIF 的缓冲不够时退回零拷贝（不带 EXIF），照片不丢。
     */
    film_blob_t *raw = film_meta_make_blob(shot, FILM_META_RAW, jpeg, jpeg_size);
    if (raw) {
        heap_caps_free(jpeg);
    } else {
        ESP_LOGW(TAG, "RAW without EXIF (no memory for the copy)");
        raw = film_blob_wrap(jpeg, jpeg_size);   /* 失败时 wrap 已释放 jpeg */
    }
    esp_err_t enqueue_err = raw ? ESP_OK : ESP_ERR_NO_MEM;
    if (enqueue_err == ESP_OK) {
        enqueue_err = film_writer_raw(lab->writer, root, shot, raw);
    }
    if (enqueue_err != ESP_OK) {
        fail_capture(lab, shot->photo_id, enqueue_err, NULL, proof_px);
        return;
    }
    /* 小样阶段结束前不开冲成片（ready=false），小样要抢在成片之前占用暗房 */
    const pending_t item = { .shot = *shot, .awb = still->awb };
    pending_set(lab, &item);
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    lab->capturing = false;
    xSemaphoreGive(lab->lock);
    const film_event_t done = { .type = FILM_EVT_SHUTTER_DONE, .photo_id = shot->photo_id };
    push_event(lab, &done);
    /* 没有小样也要投递一个空任务：它负责放行成片冲洗，也把睡着的暗房叫醒 */
    job_t job = { .shot = *shot, .proof_only = true };
    if (proof) {
        job.proof = *proof;
    }
    if (xQueueSend(lab->jobs, &job, 0) != pdTRUE) {
        heap_caps_free(proof_px);   /* 队列只有这一个在途任务，不会满 */
        ESP_LOGE(TAG, "proof job queue full");
    }
}

esp_err_t film_lab_redevelop(film_lab_handle_t lab, const film_shot_t *shot)
{
    xSemaphoreTake(lab->lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!lab->warmed) {
        err = FILM_ERR_STORAGE_PREPARING;
    } else if (lab->capturing || lab->redeveloping || lab->developing || lab->has_pending) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!lab->root) {
        err = FILM_ERR_STORAGE_FULL;
    } else {
        lab->redeveloping = true;
    }
    xSemaphoreGive(lab->lock);
    ESP_RETURN_ON_ERROR(err, TAG, "redevelop busy");
    const job_t job = { .shot = *shot, .raw_id = shot->source_id, .copy_raw = !shot->preview_only };
    if (xQueueSend(lab->jobs, &job, 0) != pdTRUE) {
        xSemaphoreTake(lab->lock, portMAX_DELAY);
        lab->redeveloping = false;
        xSemaphoreGive(lab->lock);
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
        .full_format = FILM_IMAGE_YUV420,   /* 全分辨率 RGB888 要 9 MB 以上，PSRAM 放不下 */
    };
    ESP_RETURN_ON_ERROR(film_darkroom_create(&darkroom, &lab->darkroom), TAG, "darkroom");
    lab->encoder_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(lab->encoder_lock, ESP_ERR_NO_MEM, TAG, "encoder lock");
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
    if (lab->encoder_lock) {
        vSemaphoreDelete(lab->encoder_lock);
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

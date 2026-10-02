#include "film_writer.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "film_port.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "film_writer";

#define WRITER_TASK_STACK   6144
#define WRITER_QUEUE_LEN    8       /*!< 一张照片最多 5 项（RAW、JPEG、THM、SCR、commit） */
#define WRITE_CHUNK         (16 * 1024)   /*!< 每次 fwrite 的上限：LittleFS 写一段时会锁住整个分区 */
#define IDLE_BIT            BIT0
#define BLOB_CAPS           (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

/* ---------------------------------------------------------------- 引用计数缓冲 */

struct film_blob_t {
    void *data;
    size_t size;
    atomic_int refs;
};

film_blob_t *film_blob_wrap(void *data, size_t size)
{
    if (!data) {
        return NULL;
    }
    film_blob_t *blob = heap_caps_malloc(sizeof(*blob), BLOB_CAPS);
    if (!blob) {
        heap_caps_free(data);
        return NULL;
    }
    blob->data = data;
    blob->size = size;
    atomic_init(&blob->refs, 1);
    return blob;
}

film_blob_t *film_blob_ref(film_blob_t *blob)
{
    if (blob) {
        atomic_fetch_add(&blob->refs, 1);
    }
    return blob;
}

void film_blob_unref(film_blob_t *blob)
{
    if (blob && atomic_fetch_sub(&blob->refs, 1) == 1) {
        heap_caps_free(blob->data);
        heap_caps_free(blob);
    }
}

void *film_blob_data(const film_blob_t *blob)
{
    return blob->data;
}

size_t film_blob_size(const film_blob_t *blob)
{
    return blob->size;
}

/* ---------------------------------------------------------------- 写文件 */

esp_err_t film_writer_write_path(const char *path, const uint8_t *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s: errno=%d", path, errno);
        return errno == ENOSPC ? FILM_ERR_STORAGE_FULL : ESP_FAIL;
    }
    int write_errno = 0;
    size_t done = 0;
    while (done < size) {
        const size_t n = size - done < WRITE_CHUNK ? size - done : WRITE_CHUNK;
        if (fwrite(data + done, 1, n, f) != n) {
            write_errno = errno;
            break;
        }
        done += n;
    }
    const bool closed = fclose(f) == 0;
    if (done < size || !closed) {
        if (!write_errno) {
            write_errno = errno;
        }
        ESP_LOGE(TAG, "write %s: errno=%d", path, write_errno);
        remove(path);
        return write_errno == ENOSPC ? FILM_ERR_STORAGE_FULL : ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- 任务 */

typedef enum {
    ITEM_FILE = 0,      /*!< 整文件（RAW / JPEG） */
    ITEM_PREVIEW,       /*!< .THM / .SCR */
    ITEM_COMMIT,
} item_kind_t;

typedef struct {
    item_kind_t kind;
    film_file_kind_t file;
    const char *root;           /*!< 照片目录（film_storage 的静态字符串） */
    film_photo_t meta;
    film_blob_t *blob;
    uint16_t width;
    uint16_t height;
    esp_err_t err;              /*!< 仅 COMMIT：暗房的结果 */
} item_t;

struct film_writer_t {
    film_writer_config_t config;
    TaskHandle_t task;
    QueueHandle_t items;
    EventGroupHandle_t state;   /*!< IDLE_BIT：队列为空且没有正在写的项 */
    SemaphoreHandle_t lock;     /*!< 保护 pending */
    unsigned pending;           /*!< 已入队还没写完的项数 */

    /* 以下只在写盘任务里使用：当前这张照片的写盘结果 */
    esp_err_t photo_err;
    int64_t photo_t0;
};

static void remove_files(const char *root, uint32_t id)
{
    static const film_file_kind_t kinds[] = { FILM_FILE_SCREEN, FILM_FILE_THUMB, FILM_FILE_JPEG, FILM_FILE_RAW };
    char path[FILM_PATH_MAX];
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        film_library_path(root, id, kinds[i], path, sizeof(path));
        remove(path);
    }
}

static void push_event(film_writer_handle_t w, const film_event_t *ev)
{
    if (xQueueSend(w->config.events, ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropping type %d", ev->type);
    }
}

static void finish_photo(film_writer_handle_t w, const item_t *item)
{
    const esp_err_t err = item->err != ESP_OK ? item->err : w->photo_err;
    if (err != ESP_OK) {
        remove_files(item->root, item->meta.id);
    }
    if (item->err == ESP_OK) {
        const film_event_t ev = err == ESP_OK
                                    ? (film_event_t){ .type = FILM_EVT_SAVED, .photo_id = item->meta.id,
                                                      .meta = item->meta }
                                    : (film_event_t){ .type = FILM_EVT_SHOT_FAILED, .photo_id = item->meta.id,
                                                      .err = err };
        push_event(w, &ev);
    }
    ESP_LOGI(TAG, "photo %lu %s in %lld ms", (unsigned long)item->meta.id,
             err == ESP_OK ? "saved" : esp_err_to_name(err),
             w->photo_t0 ? (long long)((esp_timer_get_time() - w->photo_t0) / 1000) : 0LL);
    w->photo_err = ESP_OK;
    w->photo_t0 = 0;
}

static void process(film_writer_handle_t w, item_t *item)
{
    if (item->kind == ITEM_COMMIT) {
        finish_photo(w, item);
        return;
    }
    if (!w->photo_t0) {
        w->photo_t0 = esp_timer_get_time();
    }
    /* 这张照片已经有一项失败：后面的就不必再写了，commit 时统一清理 */
    if (w->photo_err == ESP_OK) {
        char path[FILM_PATH_MAX];
        if (item->kind == ITEM_FILE) {
            film_library_path(item->root, item->meta.id, item->file, path, sizeof(path));
            w->photo_err = film_writer_write_path(path, film_blob_data(item->blob), film_blob_size(item->blob));
        } else {
            w->photo_err = film_library_write_raw(item->root, &item->meta, item->file, film_blob_data(item->blob),
                                                  item->width, item->height);
        }
    }
    film_blob_unref(item->blob);
}

static void item_done(film_writer_handle_t w)
{
    xSemaphoreTake(w->lock, portMAX_DELAY);
    if (--w->pending == 0) {
        xEventGroupSetBits(w->state, IDLE_BIT);
    }
    xSemaphoreGive(w->lock);
}

static void writer_task(void *arg)
{
    film_writer_handle_t w = arg;
    for (;;) {
        item_t item;
        if (xQueueReceive(w->items, &item, portMAX_DELAY) == pdTRUE) {
            process(w, &item);
            item_done(w);
        }
    }
}

/* ---------------------------------------------------------------- 入队 */

static esp_err_t enqueue(film_writer_handle_t w, const item_t *item)
{
    xSemaphoreTake(w->lock, portMAX_DELAY);
    if (w->pending++ == 0) {
        xEventGroupClearBits(w->state, IDLE_BIT);
    }
    xSemaphoreGive(w->lock);
    if (xQueueSend(w->items, item, 0) != pdTRUE) {
        item_done(w);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t film_writer_file(film_writer_handle_t writer, const char *root, const film_photo_t *meta,
                           film_file_kind_t kind, film_blob_t *blob)
{
    if (!writer || !root || !meta || !blob) {
        film_blob_unref(blob);
        return ESP_ERR_INVALID_ARG;
    }
    const item_t item = { .kind = ITEM_FILE, .file = kind, .root = root, .meta = *meta, .blob = blob };
    const esp_err_t err = enqueue(writer, &item);
    if (err != ESP_OK) {
        film_blob_unref(blob);
    }
    return err;
}

esp_err_t film_writer_preview(film_writer_handle_t writer, const char *root, const film_photo_t *meta,
                              film_file_kind_t kind, film_blob_t *blob, uint16_t width, uint16_t height)
{
    if (!writer || !root || !meta || !blob) {
        film_blob_unref(blob);
        return ESP_ERR_INVALID_ARG;
    }
    const item_t item = {
        .kind = ITEM_PREVIEW, .file = kind, .root = root, .meta = *meta, .blob = blob, .width = width, .height = height,
    };
    const esp_err_t err = enqueue(writer, &item);
    if (err != ESP_OK) {
        film_blob_unref(blob);
    }
    return err;
}

esp_err_t film_writer_commit(film_writer_handle_t writer, const char *root, const film_photo_t *meta, esp_err_t err)
{
    if (!writer || !root || !meta) {
        return ESP_ERR_INVALID_ARG;
    }
    const item_t item = { .kind = ITEM_COMMIT, .root = root, .meta = *meta, .err = err };
    const esp_err_t qerr = enqueue(writer, &item);
    if (qerr != ESP_OK) {
        /* 队列满（不应发生）：等前面的写完再同步清理，免得留下半张照片 */
        (void)film_writer_wait_idle(writer, portMAX_DELAY);
        remove_files(root, meta->id);
    }
    return qerr;
}

bool film_writer_wait_idle(film_writer_handle_t writer, uint32_t timeout_ms)
{
    const TickType_t ticks = timeout_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xEventGroupWaitBits(writer->state, IDLE_BIT, pdFALSE, pdTRUE, ticks) & IDLE_BIT) != 0;
}

/* ---------------------------------------------------------------- 创建 */

esp_err_t film_writer_start(const film_writer_config_t *config, film_writer_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && config->events && ret_handle, ESP_ERR_INVALID_ARG, TAG, "bad args");
    film_writer_handle_t w = calloc(1, sizeof(*w));
    ESP_RETURN_ON_FALSE(w, ESP_ERR_NO_MEM, TAG, "writer");
    w->config = *config;
    w->items = xQueueCreate(WRITER_QUEUE_LEN, sizeof(item_t));
    w->state = xEventGroupCreate();
    w->lock = xSemaphoreCreateMutex();
    esp_err_t err = w->items && w->state && w->lock ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        xEventGroupSetBits(w->state, IDLE_BIT);
        err = xTaskCreatePinnedToCore(writer_task, "film_writer", WRITER_TASK_STACK, w, config->priority, &w->task,
                                      tskNO_AFFINITY) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        film_writer_delete(w);
        return err;
    }
    *ret_handle = w;
    return ESP_OK;
}

void film_writer_delete(film_writer_handle_t w)
{
    if (!w) {
        return;
    }
    if (w->task) {
        vTaskDelete(w->task);
    }
    if (w->items) {
        vQueueDelete(w->items);
    }
    if (w->state) {
        vEventGroupDelete(w->state);
    }
    if (w->lock) {
        vSemaphoreDelete(w->lock);
    }
    free(w);
}

/*
 * 相册索引与照片文件读写。
 *
 * INDEX.BIN 格式：8 字节头（"MFIX" + 版本 + 保留）后接若干条 32 字节 film_photo_t，按编号升序追加。
 * 新增照片只追加一条；删除时整体重写（先写临时文件再改名）。
 */
#include "film_library.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

_Static_assert(sizeof(film_photo_t) == 32, "film_photo_t is an on-disk record");

#define INDEX_NAME "INDEX.BIN"
#define INDEX_MAGIC "MFIX"
#define INDEX_VERSION 1
#define INDEX_HEADER_LEN 8
#define INITIAL_CAPACITY 64
#define ID_DIGITS_FMT "MF%06u"

struct film_library_t {
    char root[FILM_PATH_MAX];
    film_photo_t *photos;   /*!< 按编号升序 */
    size_t count;
    size_t capacity;
    uint32_t next_id;
};

void film_library_path(const char *root, uint32_t id, film_file_kind_t kind, char *buf, size_t len)
{
    static const char *const suffix[] = {
        [FILM_FILE_JPEG] = ".JPG",
        [FILM_FILE_RAW] = "_RAW.JPG",
        [FILM_FILE_SCREEN] = ".SCR",
        [FILM_FILE_THUMB] = ".THM",
    };
    snprintf(buf, len, "%s/" ID_DIGITS_FMT "%s", root, (unsigned)id, suffix[kind]);
}

static void index_path(const film_library_handle_t lib, char *buf, size_t len)
{
    snprintf(buf, len, "%s/%s", lib->root, INDEX_NAME);
}

static esp_err_t ensure_capacity(film_library_handle_t lib, size_t need)
{
    if (need <= lib->capacity) {
        return ESP_OK;
    }
    if (need > FILM_LIBRARY_MAX_PHOTOS) {
        return ESP_ERR_INVALID_SIZE;
    }
    size_t cap = lib->capacity ? lib->capacity : INITIAL_CAPACITY;
    while (cap < need) {
        cap *= 2;
    }
    if (cap > FILM_LIBRARY_MAX_PHOTOS) {
        cap = FILM_LIBRARY_MAX_PHOTOS;
    }
    film_photo_t *grown = realloc(lib->photos, cap * sizeof(film_photo_t));
    if (!grown) {
        return ESP_ERR_NO_MEM;
    }
    lib->photos = grown;
    lib->capacity = cap;
    return ESP_OK;
}

static int compare_id(const void *a, const void *b)
{
    const uint32_t ia = ((const film_photo_t *)a)->id;
    const uint32_t ib = ((const film_photo_t *)b)->id;
    return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

/** 记录是否像一张合法照片（防止损坏数据进入界面） */
static bool photo_valid(const film_photo_t *p)
{
    return p->id != 0 && p->screen_w > 0 && p->screen_w <= 1024 && p->screen_h > 0 && p->screen_h <= 1024 &&
           p->thumb_w > 0 && p->thumb_w <= 512 && p->thumb_h > 0 && p->thumb_h <= 512;
}

static void append_loaded(film_library_handle_t lib, const film_photo_t *p)
{
    if (photo_valid(p) && ensure_capacity(lib, lib->count + 1) == ESP_OK) {
        lib->photos[lib->count++] = *p;
    }
}

static bool load_index(film_library_handle_t lib)
{
    char path[FILM_PATH_MAX + 16];
    index_path(lib, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    char header[INDEX_HEADER_LEN];
    bool ok = fread(header, 1, sizeof(header), f) == sizeof(header) && memcmp(header, INDEX_MAGIC, 4) == 0 &&
              header[4] == INDEX_VERSION;
    film_photo_t rec;
    while (ok && fread(&rec, sizeof(rec), 1, f) == 1) {
        append_loaded(lib, &rec);
    }
    fclose(f);
    return ok;
}

/** 索引缺失或损坏时，扫描每张 .SCR 的文件头重建 */
static void rebuild_from_files(film_library_handle_t lib)
{
    DIR *dir = opendir(lib->root);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        unsigned id = 0;
        char tail[8] = { 0 };
        if (sscanf(entry->d_name, ID_DIGITS_FMT "%7s", &id, tail) != 2 || strcmp(tail, ".SCR") != 0) {
            continue;
        }
        char path[FILM_PATH_MAX + 32];
        film_library_path(lib->root, id, FILM_FILE_SCREEN, path, sizeof(path));
        FILE *f = fopen(path, "rb");
        if (!f) {
            continue;
        }
        film_photo_t rec;
        if (fread(&rec, sizeof(rec), 1, f) == 1 && rec.id == id) {
            append_loaded(lib, &rec);
        }
        fclose(f);
    }
    closedir(dir);
}

/** 把整个索引重写一遍（临时文件 + 改名） */
static esp_err_t write_index(film_library_handle_t lib)
{
    char path[FILM_PATH_MAX + 16];
    char tmp[FILM_PATH_MAX + 20];
    index_path(lib, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    const char header[INDEX_HEADER_LEN] = { 'M', 'F', 'I', 'X', INDEX_VERSION, 0, 0, 0 };
    bool ok = fwrite(header, 1, sizeof(header), f) == sizeof(header);
    if (ok && lib->count) {
        ok = fwrite(lib->photos, sizeof(film_photo_t), lib->count, f) == lib->count;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t film_library_create(const film_library_config_t *config, film_library_handle_t *ret_handle)
{
    if (!config || !config->root || !ret_handle || strlen(config->root) >= FILM_PATH_MAX - 24) {
        return ESP_ERR_INVALID_ARG;
    }
    struct stat st;
    if (stat(config->root, &st) != 0 && mkdir(config->root, 0775) != 0 && errno != EEXIST) {
        return ESP_FAIL;
    }
    film_library_handle_t lib = calloc(1, sizeof(*lib));
    if (!lib) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(lib->root, sizeof(lib->root), "%s", config->root);
    if (!load_index(lib)) {
        lib->count = 0;
        rebuild_from_files(lib);
        if (lib->count) {
            qsort(lib->photos, lib->count, sizeof(film_photo_t), compare_id);
        }
        (void)write_index(lib);
    } else if (lib->count > 1) {
        qsort(lib->photos, lib->count, sizeof(film_photo_t), compare_id);
    }
    lib->next_id = lib->count ? lib->photos[lib->count - 1].id + 1 : 1;
    *ret_handle = lib;
    return ESP_OK;
}

void film_library_delete(film_library_handle_t handle)
{
    if (handle) {
        free(handle->photos);
        free(handle);
    }
}

size_t film_library_count(film_library_handle_t handle)
{
    return handle ? handle->count : 0;
}

const film_photo_t *film_library_get(film_library_handle_t handle, size_t index)
{
    if (!handle || index >= handle->count) {
        return NULL;
    }
    return &handle->photos[handle->count - 1 - index];
}

const film_photo_t *film_library_find(film_library_handle_t handle, uint32_t id, size_t *ret_index)
{
    if (!handle) {
        return NULL;
    }
    size_t lo = 0, hi = handle->count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (handle->photos[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo >= handle->count || handle->photos[lo].id != id) {
        return NULL;
    }
    if (ret_index) {
        *ret_index = handle->count - 1 - lo;
    }
    return &handle->photos[lo];
}

uint32_t film_library_reserve_id(film_library_handle_t handle)
{
    return handle ? handle->next_id++ : 0;
}

esp_err_t film_library_add(film_library_handle_t handle, const film_photo_t *photo)
{
    if (!handle || !photo || !photo_valid(photo) || film_library_find(handle, photo->id, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_capacity(handle, handle->count + 1);
    if (err != ESP_OK) {
        return err;
    }
    /* 编号几乎总是最大的，插入排序只需后移极少元素 */
    size_t pos = handle->count;
    while (pos > 0 && handle->photos[pos - 1].id > photo->id) {
        handle->photos[pos] = handle->photos[pos - 1];
        --pos;
    }
    handle->photos[pos] = *photo;
    handle->count++;
    if (photo->id >= handle->next_id) {
        handle->next_id = photo->id + 1;
    }
    if (pos != handle->count - 1) {
        return write_index(handle);
    }
    char path[FILM_PATH_MAX + 16];
    index_path(handle, path, sizeof(path));
    FILE *f = fopen(path, "ab");
    if (!f) {
        return write_index(handle);
    }
    const bool ok = fwrite(photo, sizeof(*photo), 1, f) == 1;
    return (fclose(f) == 0 && ok) ? ESP_OK : write_index(handle);
}

/** 只从内存索引里摘掉一张（不写盘）；不在库里返回 false */
static bool drop_entry(film_library_handle_t handle, uint32_t id)
{
    size_t index;
    if (!film_library_find(handle, id, &index)) {
        return false;
    }
    const size_t pos = handle->count - 1 - index;
    memmove(&handle->photos[pos], &handle->photos[pos + 1], (handle->count - pos - 1) * sizeof(film_photo_t));
    handle->count--;
    return true;
}

/** 删掉一张照片的全部文件（原片、成片、缩略图）；文件不存在不算错 */
static void remove_files(film_library_handle_t handle, uint32_t id)
{
    char path[FILM_PATH_MAX + 32];
    for (int kind = FILM_FILE_JPEG; kind <= FILM_FILE_THUMB; ++kind) {
        film_library_path(handle->root, id, (film_file_kind_t)kind, path, sizeof(path));
        remove(path);
    }
}

esp_err_t film_library_remove(film_library_handle_t handle, uint32_t id)
{
    if (!drop_entry(handle, id)) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t err = write_index(handle);
    remove_files(handle, id);
    return err;
}

esp_err_t film_library_remove_many(film_library_handle_t handle, const uint32_t *ids, size_t count,
                                   size_t *ret_removed)
{
    if (ret_removed) {
        *ret_removed = 0;
    }
    if (!handle || (!ids && count)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t removed = 0;
    for (size_t i = 0; i < count; ++i) {
        removed += drop_entry(handle, ids[i]) ? 1 : 0;
    }
    if (removed == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* 索引只写一次：NAND 上每写一次索引都要整页擦写，逐张删会慢得多 */
    const esp_err_t err = write_index(handle);
    for (size_t i = 0; i < count; ++i) {
        remove_files(handle, ids[i]);
    }
    if (ret_removed) {
        *ret_removed = removed;
    }
    return err;
}

esp_err_t film_library_load_pixels(film_library_handle_t handle, uint32_t id, film_file_kind_t kind, uint16_t *buf,
                                   size_t capacity, uint16_t *ret_w, uint16_t *ret_h)
{
    if (!handle || !buf || (kind != FILM_FILE_SCREEN && kind != FILM_FILE_THUMB)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[FILM_PATH_MAX + 32];
    film_library_path(handle->root, id, kind, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    film_photo_t header;
    esp_err_t err = ESP_FAIL;
    if (fread(&header, sizeof(header), 1, f) == 1 && header.id == id) {
        const uint16_t w = kind == FILM_FILE_SCREEN ? header.screen_w : header.thumb_w;
        const uint16_t h = kind == FILM_FILE_SCREEN ? header.screen_h : header.thumb_h;
        const size_t n = (size_t)w * h;
        if (n == 0 || n > capacity) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (fread(buf, sizeof(uint16_t), n, f) == n) {
            *ret_w = w;
            *ret_h = h;
            err = ESP_OK;
        }
    }
    fclose(f);
    return err;
}

esp_err_t film_library_write_raw(const char *root, const film_photo_t *photo, film_file_kind_t kind,
                                 const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (!root || !photo || !pixels || (kind != FILM_FILE_SCREEN && kind != FILM_FILE_THUMB)) {
        return ESP_ERR_INVALID_ARG;
    }
    char path[FILM_PATH_MAX + 32];
    char tmp[FILM_PATH_MAX + 40];
    film_library_path(root, photo->id, kind, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    const size_t n = (size_t)width * height;
    bool ok = fwrite(photo, sizeof(*photo), 1, f) == 1 && fwrite(pixels, sizeof(uint16_t), n, f) == n;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return errno == ENOSPC ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }
    return ESP_OK;
}

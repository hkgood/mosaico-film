#include "film_pending.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_library.h"

#define PENDING_MAGIC     "MFPD"
#define PENDING_VERSION   1
#define PENDING_FILE_ROOM 12

/* 标志位 */
#define FLAG_INSTANT      0x01
#define FLAG_CROP_43      0x02
#define FLAG_DATE_STAMP   0x04
#define FLAG_LIGHT_LEAK   0x08

#define EXPOSURE_MILLI_MAX 4000     /*!< 标记里允许的 |曝光补偿|（毫 EV）；界面范围 ±2 EV，留一倍余量判损坏 */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, (uint32_t)v);
    put_u32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get_u64(const uint8_t *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/** 标准 CRC-32（多项式 0xEDB88320），按位算：36 字节不值得查表 */
static uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

void film_pending_encode(const film_shot_t *shot, uint8_t out[FILM_PENDING_BYTES])
{
    memset(out, 0, FILM_PENDING_BYTES);
    memcpy(out, PENDING_MAGIC, 4);
    out[4] = PENDING_VERSION;
    out[5] = (uint8_t)((shot->instant ? FLAG_INSTANT : 0) | (shot->crop_43 ? FLAG_CROP_43 : 0) |
                       (shot->date_stamp ? FLAG_DATE_STAMP : 0) | (shot->light_leak ? FLAG_LIGHT_LEAK : 0));
    out[6] = (uint8_t)shot->rot;
    out[7] = (uint8_t)shot->film;
    put_u32(out + 8, shot->photo_id);
    put_u32(out + 12, shot->seed);
    int32_t milli = (int32_t)(shot->exposure_ev * 1000.0f + (shot->exposure_ev < 0 ? -0.5f : 0.5f));
    put_u32(out + 16, (uint32_t)milli);
    out[20] = (uint8_t)shot->roll;
    out[21] = (uint8_t)(shot->roll >> 8);
    out[22] = shot->frame;
    put_u64(out + 24, (uint64_t)shot->time);
    put_u32(out + 32, crc32(out, 32));
}

bool film_pending_decode(const uint8_t *buf, size_t len, film_shot_t *ret)
{
    if (!buf || !ret || len != FILM_PENDING_BYTES || memcmp(buf, PENDING_MAGIC, 4) != 0 ||
        buf[4] != PENDING_VERSION || get_u32(buf + 32) != crc32(buf, 32)) {
        return false;
    }
    const int32_t milli = (int32_t)get_u32(buf + 16);
    if (buf[7] >= FILM_ID_COUNT || buf[6] > (uint8_t)GFX_ROT_270 || get_u32(buf + 8) == 0 ||
        milli > EXPOSURE_MILLI_MAX || milli < -EXPOSURE_MILLI_MAX) {
        return false;
    }
    *ret = (film_shot_t){
        .photo_id = get_u32(buf + 8),
        .film = (film_id_t)buf[7],
        .exposure_ev = (float)milli / 1000.0f,
        .instant = (buf[5] & FLAG_INSTANT) != 0,
        .crop_43 = (buf[5] & FLAG_CROP_43) != 0,
        .date_stamp = (buf[5] & FLAG_DATE_STAMP) != 0,
        .light_leak = (buf[5] & FLAG_LIGHT_LEAK) != 0,
        .rot = (gfx_rot_t)buf[6],
        .seed = get_u32(buf + 12),
        .roll = (uint16_t)(buf[20] | (buf[21] << 8)),
        .frame = buf[22],
        .time = (int64_t)get_u64(buf + 24),
    };
    return true;
}

esp_err_t film_pending_read(const char *root, uint32_t id, film_shot_t *ret)
{
    char path[FILM_PATH_MAX + PENDING_FILE_ROOM];
    film_library_path(root, id, FILM_FILE_PENDING, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t buf[FILM_PENDING_BYTES + 1];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    film_shot_t shot;
    if (!film_pending_decode(buf, n, &shot) || shot.photo_id != id) {
        return ESP_FAIL;
    }
    *ret = shot;
    return ESP_OK;
}

void film_pending_remove(const char *root, uint32_t id)
{
    char path[FILM_PATH_MAX + PENDING_FILE_ROOM];
    film_library_path(root, id, FILM_FILE_PENDING, path, sizeof(path));
    remove(path);
}

bool film_pending_raw_complete(const char *root, uint32_t id)
{
    char path[FILM_PATH_MAX + PENDING_FILE_ROOM];
    film_library_path(root, id, FILM_FILE_RAW, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    uint8_t head[2] = { 0 }, tail[2] = { 0 };
    bool ok = fread(head, 1, 2, f) == 2 && head[0] == 0xFF && head[1] == 0xD8 && fseek(f, -2, SEEK_END) == 0 &&
              fread(tail, 1, 2, f) == 2;
    fclose(f);
    /* DVP 帧尾可能带填充字节（见 film_still.c），拍摄时已把 JPEG 截到 EOI；这里只要求 EOI 结尾 */
    return ok && tail[0] == 0xFF && tail[1] == 0xD9;
}

static void remove_all_files(const char *root, uint32_t id)
{
    char path[FILM_PATH_MAX + PENDING_FILE_ROOM];
    for (int kind = FILM_FILE_JPEG; kind <= FILM_FILE_PENDING; ++kind) {
        film_library_path(root, id, (film_file_kind_t)kind, path, sizeof(path));
        remove(path);
    }
}

static int compare_id(const void *a, const void *b)
{
    const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : (x > y);
}

/** 一遍目录扫描：最大的 .SCR 编号，以及所有 .PND / _RAW.JPG 的编号（各自最多 cap 个，多出的忽略） */
typedef struct {
    uint32_t max_scr;
    uint32_t *pnd;
    size_t pnd_count;
    uint32_t *raw;
    size_t raw_count;
    size_t cap;
} scan_t;

static void scan_dir(const char *root, scan_t *s)
{
    DIR *dir = opendir(root);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        unsigned id = 0;
        char tail[12] = { 0 };
        if (sscanf(entry->d_name, "MF%06u%11s", &id, tail) != 2) {
            continue;
        }
        if (strcmp(tail, ".SCR") == 0) {
            if (id > s->max_scr) {
                s->max_scr = id;
            }
        } else if (strcmp(tail, ".PND") == 0 && s->pnd_count < s->cap) {
            s->pnd[s->pnd_count++] = id;
        } else if (strcmp(tail, "_RAW.JPG") == 0) {
            /* RAW 太多时，只保留编号大的（孤儿一定是最新的）：先收满，之后在 prune 里按需处理 */
            if (s->raw_count < s->cap) {
                s->raw[s->raw_count++] = id;
            } else {
                uint32_t *lowest = &s->raw[0];
                for (size_t i = 1; i < s->raw_count; ++i) {
                    if (s->raw[i] < *lowest) {
                        lowest = &s->raw[i];
                    }
                }
                if (id > *lowest) {
                    *lowest = id;
                }
            }
        }
    }
    closedir(dir);
}

size_t film_pending_recover(const char *root, film_shot_t *shots, size_t cap)
{
    if (!root || !shots || cap == 0) {
        return 0;
    }
    /* 孤儿与标记各按 cap*4 个编号的容量收集（实际同时待冲洗的只有几张） */
    const size_t scan_cap = cap * 4;
    uint32_t *ids = calloc(scan_cap * 2, sizeof(uint32_t));
    if (!ids) {
        return 0;
    }
    scan_t s = { .pnd = ids, .raw = ids + scan_cap, .cap = scan_cap };
    scan_dir(root, &s);

    /* 比所有 .SCR 都新、没有 .PND 的 RAW：写到一半断电的孤儿 */
    qsort(s.pnd, s.pnd_count, sizeof(uint32_t), compare_id);
    for (size_t i = 0; i < s.raw_count; ++i) {
        const uint32_t id = s.raw[i];
        if (id <= s.max_scr) {
            continue;
        }
        bool has_marker = false;
        for (size_t k = 0; k < s.pnd_count && !has_marker; ++k) {
            has_marker = s.pnd[k] == id;
        }
        if (!has_marker) {
            remove_all_files(root, id);
        }
    }

    size_t count = 0;
    for (size_t i = 0; i < s.pnd_count && count < cap; ++i) {
        const uint32_t id = s.pnd[i];
        char scr[FILM_PATH_MAX + PENDING_FILE_ROOM];
        film_library_path(root, id, FILM_FILE_SCREEN, scr, sizeof(scr));
        FILE *done = fopen(scr, "rb");
        if (done) {
            /* 已经有 .SCR：冲洗其实完成了，只是删 .PND 之前断电；标记没用了，照片文件不能动 */
            fclose(done);
            film_pending_remove(root, id);
            continue;
        }
        film_shot_t shot;
        if (film_pending_read(root, id, &shot) == ESP_OK && film_pending_raw_complete(root, id)) {
            shots[count++] = shot;
        } else {
            remove_all_files(root, id);   /* 标记损坏或 RAW 不完整：这张照片无法恢复 */
        }
    }
    free(ids);
    return count;
}

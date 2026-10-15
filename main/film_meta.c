#include "film_meta.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "film_exif.h"
#include "film_filter.h"

static const char *TAG = "film_meta";

#define FILE_CAPS       (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define EXIF_MAKE       "Espressif ESP-Mosaico"
#define EXIF_MODEL_M6   "Mosaico Film M6"
#define EXIF_MODEL_SX70 "Mosaico Film SX-70"

static const char *film_key(film_id_t film)
{
    const char *key = film < FILM_ID_COUNT ? film_filter_get_key(film) : NULL;
    return key ? key : "none";
}

/**
 * 机器可读的拍摄参数，足以复现冲洗：胶卷、曝光、颗粒/漏光种子、日期戳、原片校正、重新冲洗的来源。
 * 全部是 ASCII，字段名保持稳定（给后期工具用）。
 */
static void make_comment(const film_shot_t *shot, film_meta_kind_t kind, char *out, size_t size)
{
    snprintf(out, size,
             "{\"film\":\"%s\",\"ev\":%.2f,\"seed\":%lu,\"leak\":%d,\"stamp\":%d,\"balance\":%lu,"
             "\"source_id\":%lu,\"body\":\"%s\",\"hold\":%d,\"roll\":%u,\"frame\":%u,\"src\":\"%s\"}",
             film_key(shot->film), (double)shot->exposure_ev, (unsigned long)shot->seed, shot->light_leak ? 1 : 0,
             shot->date_stamp ? 1 : 0, (unsigned long)shot->balance, (unsigned long)shot->source_id,
             shot->instant ? "sx70" : "m6", (int)shot->rot, (unsigned)shot->roll, (unsigned)shot->frame,
             kind == FILM_META_RAW ? "raw" : "jpg");
}

film_blob_t *film_meta_make_blob(const film_shot_t *shot, film_meta_kind_t kind, const uint8_t *jpeg, size_t size)
{
    uint16_t width = 0, height = 0;   /* 读不到就不写（film_exif 对 0 不写尺寸标签） */
    (void)film_exif_jpeg_size(jpeg, size, &width, &height);
    char software[48], description[96], comment[320];
    const esp_app_desc_t *app = esp_app_get_description();
    snprintf(software, sizeof(software), "mosaico_film %s", app ? app->version : "?");
    snprintf(description, sizeof(description), "%s%s roll %u frame %u", film_key(shot->film),
             kind == FILM_META_RAW ? " (raw)" : "", (unsigned)shot->roll, (unsigned)shot->frame);
    make_comment(shot, kind, comment, sizeof(comment));

    const film_exif_t exif = {
        .make = EXIF_MAKE, .model = shot->instant ? EXIF_MODEL_SX70 : EXIF_MODEL_M6, .software = software, .description = description,
        .user_comment = comment,
        /* 按需求不在 EXIF 里写任何时间：time 保持 0，film_exif 不会生成时间与时区偏移标签 */
        .time = 0,
        /* 成片像素已转正（Orientation 1）；RAW 是传感器方向，按握持方向给出转正所需的取向 */
        .orientation = kind == FILM_META_RAW ? film_exif_orientation_for_raw((int)shot->rot) : FILM_EXIF_ORIENT_NORMAL,
        .width = width, .height = height,
        .has_exposure_bias = true, .exposure_bias_ev = shot->exposure_ev,
    };
    uint8_t segment[FILM_EXIF_SEGMENT_MAX];
    const size_t segment_size = film_exif_build(&exif, segment, sizeof(segment));
    const size_t total = segment_size ? film_exif_spliced_size(jpeg, size, segment_size) : 0;
    if (total == 0) {
        ESP_LOGW(TAG, "no EXIF written (segment %u bytes, jpeg valid: %d)", (unsigned)segment_size,
                 jpeg && size > 2 && jpeg[0] == 0xFF && jpeg[1] == 0xD8);
    }

    const size_t alloc = total ? total : size;
    uint8_t *copy = heap_caps_malloc(alloc, FILE_CAPS);
    if (!copy) {
        return NULL;
    }
    if (total && film_exif_splice(jpeg, size, segment, segment_size, copy, alloc) == total) {
        return film_blob_wrap(copy, total);
    }
    memcpy(copy, jpeg, size);   /* 退化：没有 EXIF，照片本身要保住 */
    return film_blob_wrap(copy, size);
}

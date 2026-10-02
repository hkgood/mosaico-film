#include "film_studio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define THUMB_BOX       FILM_DARKROOM_THUMB_MAX
#define COPY_CHUNK      4096

/* 元数据里的细分位要能原样拼回 film_balance_t */
_Static_assert(FILM_PHOTO_BALANCE_CODE_FINE_SHIFT == FILM_BALANCE_FINE_SHIFT, "balance fine bits");
_Static_assert((FILM_PHOTO_BALANCE_FINE_MASK >> FILM_PHOTO_BALANCE_FINE_SHIFT) == FILM_BALANCE_FINE_MASK,
               "balance fine mask");

void film_studio_stamp_text(int64_t time_s, char *buf, size_t len)
{
    if (time_s <= 0) {
        buf[0] = '\0';
        return;
    }
    const time_t t = (time_t)time_s;
    struct tm tm_local;
    localtime_r(&t, &tm_local);
    /* 与取景里的日期戳一致：月、日不补零（老相机日期背的写法） */
    /* 转成 uint8_t 让编译器知道每段最多 3 位，不会截断 */
    snprintf(buf, len, "'%02u %u %u", (unsigned)(uint8_t)(tm_local.tm_year % 100),
             (unsigned)(uint8_t)(tm_local.tm_mon + 1), (unsigned)(uint8_t)tm_local.tm_mday);
}

esp_err_t film_studio_copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb");
    if (!in) {
        return ESP_ERR_NOT_FOUND;
    }
    char tmp[FILM_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", to);
    FILE *out = fopen(tmp, "wb");
    uint8_t *chunk = malloc(COPY_CHUNK);
    bool ok = out && chunk;
    while (ok) {
        const size_t n = fread(chunk, 1, COPY_CHUNK, in);
        if (n == 0) {
            ok = !ferror(in);
            break;
        }
        ok = fwrite(chunk, 1, n, out) == n;
    }
    free(chunk);
    fclose(in);
    if (out) {
        ok = (fclose(out) == 0) && ok;
    }
    if (!ok || rename(tmp, to) != 0) {
        remove(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void report(const film_studio_config_t *config, film_stage_t stage)
{
    if (config->progress) {
        config->progress(config->progress_ctx, stage, 0.0f);
    }
}

static esp_err_t write_preview(const film_studio_config_t *config, const film_photo_t *meta, film_file_kind_t kind,
                               const uint16_t *pixels, int width, int height)
{
    if (config->write_preview) {
        return config->write_preview(config->ctx, meta, kind, pixels, (uint16_t)width, (uint16_t)height);
    }
    return film_library_write_raw(config->root, meta, kind, pixels, (uint16_t)width, (uint16_t)height);
}

static void remove_outputs(const char *root, uint32_t id)
{
    static const film_file_kind_t kinds[] = { FILM_FILE_JPEG, FILM_FILE_SCREEN, FILM_FILE_THUMB };
    char path[FILM_PATH_MAX];
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        film_library_path(root, id, kinds[i], path, sizeof(path));
        remove(path);
    }
}

esp_err_t film_studio_process(const film_studio_config_t *config, const film_shot_t *shot,
                              const film_darkroom_source_t *source, uint16_t *screen,
                              film_studio_result_t *ret_result)
{
    if (!config || !shot || !source || !screen || !ret_result || (!shot->preview_only && !config->root)) {
        return ESP_ERR_INVALID_ARG;
    }
    char stamp[16] = "";
    if (shot->date_stamp && shot->film < FILM_ID_COUNT) {
        film_studio_stamp_text(shot->time, stamp, sizeof(stamp));
    }
    film_balance_t balance = FILM_BALANCE_NONE;
    const film_darkroom_job_t job = {
        .film = shot->film,
        .exposure_ev = shot->exposure_ev,
        .seed = shot->seed,
        .light_leak = shot->light_leak,
        .instant = shot->instant,
        .rot = shot->rot,
        .stamp = stamp,
        .size = shot->preview_only ? FILM_DARKROOM_SCREEN : FILM_DARKROOM_FULL,
        .progress = config->progress,
        .progress_ctx = config->progress_ctx,
        .match = config->match,
        .balance = shot->balance,
        .ret_stats = config->ret_stats,
        .ret_balance = &balance,
    };
    film_darkroom_image_t image;
    esp_err_t err = film_darkroom_develop(config->darkroom, &job, source, &image);
    if (err != ESP_OK) {
        return err;
    }
    if (config->source_done) {
        config->source_done(config->ctx);
    }

    /* 屏幕图与缩略图（编码 JPEG 可能改写像素，所以先做） */
    report(config, FILM_STAGE_PREVIEWS);
    int sw, sh, tw, th;
    film_darkroom_fit(image.width, image.height, FILM_DARKROOM_SCREEN_MAX_W, FILM_DARKROOM_SCREEN_MAX_H, &sw, &sh);
    film_darkroom_fit(image.width, image.height, THUMB_BOX, THUMB_BOX, &tw, &th);
    uint32_t *scratch = malloc(sizeof(uint32_t) * 3 * FILM_DARKROOM_SCREEN_MAX_W);
    uint16_t *thumb = shot->preview_only ? NULL : malloc(sizeof(uint16_t) * THUMB_BOX * THUMB_BOX);
    if (!scratch || (!shot->preview_only && !thumb)) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    film_darkroom_downscale_565(&image, screen, sw, sh, scratch);
    memset(ret_result, 0, sizeof(*ret_result));
    ret_result->screen_w = (uint16_t)sw;
    ret_result->screen_h = (uint16_t)sh;
    ret_result->balance = balance;
    if (shot->preview_only) {
        goto done;
    }
    film_darkroom_downscale_565(&image, thumb, tw, th, scratch);

    film_photo_t *meta = &ret_result->meta;
    *meta = (film_photo_t) {
        .id = shot->photo_id,
        .time = shot->time,
        .roll = shot->roll,
        .screen_w = (uint16_t)sw,
        .screen_h = (uint16_t)sh,
        .thumb_w = (uint16_t)tw,
        .thumb_h = (uint16_t)th,
        .film = (uint8_t)shot->film,
        .frame = shot->frame,
        .rot = (uint8_t)shot->rot,
        .balance = (uint16_t)balance,
    };
    meta->flags = (uint8_t)((shot->instant ? FILM_PHOTO_INSTANT : 0) | (stamp[0] ? FILM_PHOTO_DATE : 0) |
                            (shot->source_id ? FILM_PHOTO_REDEVELOP : 0) |
                            (((balance >> FILM_BALANCE_FINE_SHIFT) & FILM_BALANCE_FINE_MASK)
                             << FILM_PHOTO_BALANCE_FINE_SHIFT));

    char path[FILM_PATH_MAX];
    film_library_path(config->root, shot->photo_id, FILM_FILE_JPEG, path, sizeof(path));
    report(config, FILM_STAGE_ENCODE);
    err = config->write_jpeg(config->ctx, &image, path, &meta->jpeg_bytes);
    report(config, FILM_STAGE_WRITE);
    if (err == ESP_OK) {
        err = write_preview(config, meta, FILM_FILE_THUMB, thumb, tw, th);
    }
    /* .SCR 最后写：相册重建索引时以它为准，有它就说明其余文件都齐了 */
    if (err == ESP_OK) {
        err = write_preview(config, meta, FILM_FILE_SCREEN, screen, sw, sh);
    }
    if (err != ESP_OK) {
        remove_outputs(config->root, shot->photo_id);
    }

done:
    free(thumb);
    free(scratch);
    film_darkroom_release(config->darkroom, &image);
    return err;
}

/*
 * film_studio：一次拍摄/重新冲洗的后半程，设备与 PC 模拟器共用。
 *
 *   原片来源 → film_darkroom 冲洗 → 屏幕图（交给界面）+ 缩略图 → JPEG 成片 → .SCR/.THM 写盘
 *
 * JPEG 编码由平台提供（设备用硬件编码器）。原片 RAW 文件由平台在拍摄时写好，
 * 重新冲洗得到的新照片用 film_studio_copy_file 复制一份原片，删除照片时互不影响。
 *
 * 线程：在平台的冲洗任务里调用，一次只处理一张（单一所有者）。
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_darkroom.h"
#include "film_library.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 屏幕图缓冲至少要这么多像素 */
#define FILM_STUDIO_SCREEN_PIXELS (FILM_DARKROOM_SCREEN_MAX_W * FILM_DARKROOM_SCREEN_MAX_H)

typedef struct {
    film_darkroom_handle_t darkroom;
    const char *root;           /*!< 照片目录 */
    /**
     * 把成片编码为 JPEG 写到 path（回调可以原地改写 image 像素，比如换通道顺序）。
     * ret_bytes 返回文件大小。
     */
    esp_err_t (*write_jpeg)(void *ctx, film_darkroom_image_t *image, const char *path, uint32_t *ret_bytes);
    /**
     * 可选：写 .THM/.SCR（RGB565 紧凑排列）。为 NULL 时直接 film_library_write_raw。
     * 平台可以复制像素后交给后台写盘（像素在回调返回后即失效）。
     */
    esp_err_t (*write_preview)(void *ctx, const film_photo_t *meta, film_file_kind_t kind, const uint16_t *pixels,
                               uint16_t width, uint16_t height);
    /** 可选：原片已读完（冲洗成功后、编码前），调用者可以趁早释放原片和解码器，降低内存峰值 */
    void (*source_done)(void *ctx);
    void *ctx;
    film_progress_fn_t progress;    /*!< 可选：冲洗进度，也转给 film_darkroom */
    void *progress_ctx;
    /** 可选：把原片校正到这组均值（拍摄时传小样的统计），为 NULL 时沿用 shot->balance */
    const film_tone_stats_t *match;
    film_tone_stats_t *ret_stats;   /*!< 可选：输出原片校正前的均值（小样冲洗时取回） */
} film_studio_config_t;

typedef struct {
    uint16_t screen_w;
    uint16_t screen_h;
    film_balance_t balance;     /*!< 实际使用的原片校正编码（低 16 位与细分位已拆进 meta） */
    film_photo_t meta;          /*!< 预览时只有尺寸有效 */
} film_studio_result_t;

/**
 * 冲洗一张并（非预览时）写盘。screen 由调用者提供，容量 FILM_STUDIO_SCREEN_PIXELS。
 * 失败时已写的文件会被清理。
 */
esp_err_t film_studio_process(const film_studio_config_t *config, const film_shot_t *shot,
                              const film_darkroom_source_t *source, uint16_t *screen,
                              film_studio_result_t *ret_result);

/** 日期戳文字（'YY M D，胶片相机日期背的格式）；time 为 0 时写入空串 */
void film_studio_stamp_text(int64_t time, char *buf, size_t len);

/** 复制文件（先写临时文件再改名） */
esp_err_t film_studio_copy_file(const char *from, const char *to);

#ifdef __cplusplus
}
#endif

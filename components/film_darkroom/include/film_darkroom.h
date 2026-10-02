/*
 * film_darkroom：把一张传感器原片"冲洗"成成片。设备与 PC 模拟器共用。
 *
 *   原片条带（传感器方向，RGB888）
 *     → 按取景框裁切、转成取景方向、按握持方向转正、缩放（双线性，逐条带完成，不需要整幅原片）
 *     → 胶卷滤镜（film_filter）
 *     → 日期戳（DSEG7 橙色光晕）
 *     → 宝丽来：放进 SX-70 相纸版式（纸纹边框，下边宽）
 *
 * 成片缓冲由调用者提供的 alloc 回调申请（设备上用 PSRAM + DMA 对齐，可直接交给硬件 JPEG 编码器），
 * 用完调用 film_darkroom_release。成片像素为 R、G、B 字节顺序。
 *
 * 线程：一个句柄同一时间只给一个任务用（单一所有者）。滤镜句柄由调用者拥有。
 * 依赖 film_assets 已绑定（日期戳字体与相纸纹理）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_balance.h"
#include "film_filter.h"
#include "film_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/** M6 取景框在传感器长边上占的比例（640 中的 360）；取景帧与成片共用这个裁切 */
#define FILM_DARKROOM_M6_CROP_NUM 9
#define FILM_DARKROOM_M6_CROP_DEN 16

/** 成片尺寸（取景方向，横拿时） */
#define FILM_DARKROOM_M6_W        1440
#define FILM_DARKROOM_M6_H        1080
/** 宝丽来相纸与其中的方形图像 */
#define FILM_DARKROOM_PRINT_W     1140
#define FILM_DARKROOM_PRINT_H     1387
#define FILM_DARKROOM_PRINT_IMAGE 1024
#define FILM_DARKROOM_PRINT_SIDE  58
#define FILM_DARKROOM_PRINT_TOP   78

/** 屏幕尺寸成片（大图浏览、显影动画）与缩略图的上限 */
#define FILM_DARKROOM_SCREEN_MAX_W 480
#define FILM_DARKROOM_SCREEN_MAX_H 360
#define FILM_DARKROOM_THUMB_MAX    160

/** 冲洗的阶段：film_darkroom 报告前三个，film_studio 报告其余的；用于进度条与计时 */
typedef enum {
    FILM_STAGE_READ = 0,        /*!< 解码原片、裁切转向缩放 */
    FILM_STAGE_FILTER,          /*!< 胶卷滤镜 */
    FILM_STAGE_FINISH,          /*!< 日期戳、相纸边 */
    FILM_STAGE_PREVIEWS,        /*!< 屏幕图与缩略图 */
    FILM_STAGE_ENCODE,          /*!< JPEG 编码并写盘 */
    FILM_STAGE_WRITE,           /*!< .THM/.SCR 写盘 */
    FILM_STAGE_COUNT,
} film_stage_t;

/** 进度回调（冲洗任务上下文）：进入阶段时 done 为 0，阶段内可以多次报告 0..1 */
typedef void (*film_progress_fn_t)(void *ctx, film_stage_t stage, float done);

/** 冲洗后输出的尺寸档 */
typedef enum {
    FILM_DARKROOM_FULL = 0,     /*!< 存盘成片 */
    FILM_DARKROOM_SCREEN,       /*!< 只要屏幕尺寸（重新冲洗页的预览），快很多 */
    FILM_DARKROOM_VIEWFINDER,   /*!< 取景画面：480×360 或 360×360，不转正、不加相纸 */
} film_darkroom_size_t;

/**
 * 冲洗增感：胶卷成片在曝光补偿之外统一提亮 1/8 档（小样、成片、重新冲洗一致，原片不变）。
 * 1/3 档时成片偏亮；1/8 档让默认胶卷的平均亮度降约 5%。
 */
#define FILM_DARKROOM_PRINT_EV  (1.0f / 8.0f)

typedef struct {
    film_id_t film;             /*!< FILM_ID_COUNT 表示不冲洗（原片） */
    float exposure_ev;
    uint32_t seed;
    bool light_leak;
    bool instant;               /*!< 宝丽来：方形构图 + 相纸 */
    gfx_rot_t rot;              /*!< 拍摄时的握持方向（与界面的 rot 一致） */
    const char *stamp;          /*!< 日期戳文字，NULL 或空串不压 */
    film_darkroom_size_t size;
    film_progress_fn_t progress;    /*!< 可选 */
    void *progress_ctx;
    /*
     * 原片白平衡与亮度对齐（见 film_balance.h），在滤镜之前做：
     * match 非空时把本次原片校正到 match 的均值（小样 → 成片），否则按 balance 编码校正。
     */
    const film_tone_stats_t *match;
    film_balance_t balance;
    film_tone_stats_t *ret_stats;   /*!< 可选：输出校正前的原片均值 */
    film_balance_t *ret_balance;    /*!< 可选：输出实际使用的校正编码 */
} film_darkroom_job_t;

/**
 * 原片来源：按从上到下的顺序一次给若干行（传感器方向，RGB888，R、G、B 字节顺序）。
 * read 返回的行在下一次 read 之前有效；读完时 *ret_count 为 0。
 */
typedef struct {
    uint16_t width;
    uint16_t height;
    void *ctx;
    esp_err_t (*read)(void *ctx, const uint8_t **ret_rows, size_t *ret_stride, int *ret_count);
} film_darkroom_source_t;

/** 成片（RGB888，R、G、B 字节顺序） */
typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    size_t stride;
    size_t size;                /*!< 缓冲总字节数 */
} film_darkroom_image_t;

typedef struct {
    film_filter_handle_t filter;        /*!< 调用者拥有，尺寸至少 1440×1440 */
    void *(*alloc)(size_t bytes, void *ctx);
    void (*free)(void *ptr, void *ctx);
    void *alloc_ctx;
} film_darkroom_config_t;

typedef struct film_darkroom_t *film_darkroom_handle_t;

esp_err_t film_darkroom_create(const film_darkroom_config_t *config, film_darkroom_handle_t *ret_handle);
void film_darkroom_delete(film_darkroom_handle_t handle);

/** 冲洗一张；成功时 ret_image 由调用者在用完后交给 film_darkroom_release */
esp_err_t film_darkroom_develop(film_darkroom_handle_t handle, const film_darkroom_job_t *job,
                                const film_darkroom_source_t *source, film_darkroom_image_t *ret_image);
void film_darkroom_release(film_darkroom_handle_t handle, film_darkroom_image_t *image);

/** 等比缩到 max_w×max_h 以内（至少 1 像素） */
void film_darkroom_fit(int width, int height, int max_w, int max_h, int *ret_w, int *ret_h);

/** 盒式滤波缩小成 RGB565（dst 紧凑排列 dst_w×dst_h）；scratch 至少 dst_w*3 个 uint32_t */
void film_darkroom_downscale_565(const film_darkroom_image_t *src, uint16_t *dst, int dst_w, int dst_h,
                                 uint32_t *scratch);

/** 只计算成片尺寸（不冲洗），用于提前申请缓冲 */
void film_darkroom_output_size(const film_darkroom_job_t *job, int *ret_w, int *ret_h);

#ifdef __cplusplus
}
#endif

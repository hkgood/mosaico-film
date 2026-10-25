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

/** M6 的 4:3 裁切框在传感器长边上占的比例（16 份中的 9 份，即 2048 中的 1152）；取景帧与成片共用这个裁切 */
#define FILM_DARKROOM_M6_CROP_NUM 9
#define FILM_DARKROOM_M6_CROP_DEN 16

/*
 * 成片尺寸：原片像素 1:1 输出，不缩放（传感器 2048×1536）。取景方向（握持方向转正之前）：
 *   M6 全画幅：1536 × 2048（宽 × 高，竖幅；握持转 90°/270° 后是 2048 × 1536）
 *   M6 4:3 裁切：1536 × 1152（横幅）
 *   SX-70 方形图像：1536 × 1536
 */
#define FILM_DARKROOM_M6_W        1536
#define FILM_DARKROOM_M6_H        1152
#define FILM_DARKROOM_M6_FULL_H   2048
/*
 * 宝丽来相纸与其中的方形图像：图像 1536 见方（原片 1:1），相纸比例沿用旧版（1024 图像、1140×1387）。
 * 图像位置取偶数（YUV420 存放按 2×2 块做色度），相纸宽高取 16 的倍数（硬件 JPEG 编码按 16×16 的 MCU 读取）。
 * 屏幕尺寸的相纸（SCREEN）仍按旧版的比例，界面里的版式不变。
 */
#define FILM_DARKROOM_PRINT_W     1712
#define FILM_DARKROOM_PRINT_H     2096
#define FILM_DARKROOM_PRINT_IMAGE 1536
#define FILM_DARKROOM_PRINT_SIDE  88
#define FILM_DARKROOM_PRINT_TOP   118
/** 任一成片的最长边上限；滤镜实例（film_filter）按这个尺寸创建 */
#define FILM_DARKROOM_MAX_EDGE    2112

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
    /*
     * M6 的取景范围：false 为传感器全画幅（默认），true 为中间 4:3 裁切。宝丽来恒为方形，忽略此项。
     * 全画幅在取景方向是 3:4 竖幅，屏幕图与取景画面相应为 360×480。
     */
    bool crop_43;
    gfx_rot_t rot;              /*!< 拍摄时的握持方向（与界面的 rot 一致） */
    const char *stamp;          /*!< 日期戳文字，NULL 或空串不压 */
    film_darkroom_size_t size;
    /*
     * 存盘成片（FILM_DARKROOM_FULL）的基准边：取景方向的图像宽，即原片短边。
     * 0 或不小于 FILM_DARKROOM_M6_W 时用标准尺寸（OV3640 原片 1:1）；原片更小（SC101IOT 的 720）时设成原片短边，
     * 成片同样按原片 1:1 输出、不放大，相纸按比例缩小。各边向下取 16 的倍数（硬件 JPEG 编码按 16×16 读取）。
     */
    uint16_t full_edge;
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

/** 成片缓冲的像素格式 */
typedef enum {
    FILM_IMAGE_RGB888 = 0,      /*!< R、G、B 字节顺序，每行 stride 字节 */
    FILM_IMAGE_YUV420,          /*!< 见 film_yuv.h：12 bit/像素，可直接交给硬件 JPEG 编码器 */
} film_image_format_t;

/** 成片（格式见 format） */
typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    size_t stride;              /*!< RGB888 每行字节数；YUV420 为 0 */
    size_t size;                /*!< 缓冲总字节数 */
    film_image_format_t format;
} film_darkroom_image_t;

/**
 * 把成片的一个矩形读成 RGB888（任意格式，供测试、缩略图等使用）。
 * 返回 0 成功，-1 越界。
 */
int film_darkroom_image_get_rgb(const film_darkroom_image_t *image, int x, int y, int w, int h, uint8_t *rgb,
                                size_t rgb_stride);

typedef struct {
    film_filter_handle_t filter;        /*!< 调用者拥有，尺寸至少 FILM_DARKROOM_MAX_EDGE 见方 */
    /*
     * 存盘成片（FILM_DARKROOM_FULL）的缓冲格式。设备上放不下 9 MB 以上的 RGB888，用 YUV420
     * （分条带滤镜，结果与整幅冲洗一致，只多一次 4:2:0 色度取样）；PC、主机测试内存充裕，用 RGB888。
     * 屏幕图与取景画面恒为 RGB888。
     */
    film_image_format_t full_format;
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

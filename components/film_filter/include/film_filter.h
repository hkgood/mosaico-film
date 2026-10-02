/*
 * 胶卷滤镜管线：把一帧 RGB888 图像按某卷胶卷的参数"冲洗"出来。
 *
 * 组件与平台无关（只依赖 C 标准库和 esp_err.h），同一份代码既跑在设备上，
 * 也能在主机上编译测试。管线顺序与设计原型 films.py 一致：
 *   曲线 → 饱和度 → 分离色调 → 高光红晕 → 暗角 → 漏光 → 颗粒
 * 像素风胶卷走单独的"下采样 + 调色板量化"分支。
 *
 * 线程模型：一个句柄同一时刻只能被一个任务使用（单一所有者），
 * 句柄内部的查找表和暂存缓冲区都归该实例所有。开启 parallel 时实例另外拥有一个
 * 工作任务，film_filter_develop 返回前它已完成全部工作，对调用者来说仍是同步调用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 胶卷编号，顺序即界面上的胶卷顺序 */
typedef enum {
    FILM_ID_GOLD = 0,   /*!< 暖金 200 */
    FILM_ID_PORTRA,     /*!< 柔肤 400 */
    FILM_ID_GREEN,      /*!< 青绿 200 */
    FILM_ID_CROSS,      /*!< 负冲 X */
    FILM_ID_BW,         /*!< 银盐 400 */
    FILM_ID_FADED,      /*!< 旧时光 77 */
    FILM_ID_NIGHT,      /*!< 夜影 800T */
    FILM_ID_PIXEL,      /*!< 像素 8-BIT */
    FILM_ID_COUNT,
} film_id_t;

typedef struct film_filter_t *film_filter_handle_t;

/** 创建参数：决定实例暂存缓冲区的大小，之后处理的图像不能超过这个尺寸 */
typedef struct {
    uint16_t max_width;
    uint16_t max_height;
    /**
     * 用一个工作任务分担下半幅图像（设备上固定在调用者之外的另一个核，优先级与调用者相同）。
     * 应在固定了核的任务里创建并调用 develop，否则两段可能挤在同一个核上。
     */
    bool parallel;
} film_filter_config_t;

/** 曝光补偿范围（EV） */
#define FILM_EXPOSURE_EV_MIN    (-2.0f)
#define FILM_EXPOSURE_EV_MAX    2.0f

/** 一次冲洗的参数（全零即"默认胶卷效果、不补偿曝光"） */
typedef struct {
    film_id_t film;
    uint32_t seed;      /*!< 颗粒与漏光位置的随机种子；同一种子结果完全可复现 */
    bool light_leak;    /*!< 是否叠加漏光 */
    bool grain;         /*!< 是否叠加颗粒（对比测试时可关闭） */
    float exposure_ev;  /*!< 曝光补偿，FILM_EXPOSURE_EV_MIN..MAX；在曲线之前作用 */
    /**
     * 取景用的快速红晕：用上一帧留下的红晕遮罩，调色与收尾合成一遍（红晕晚一帧）。
     * 同一实例上一次冲洗的胶卷和尺寸相同时才生效，否则自动走完整的两遍算法。
     * 成片必须保持 false。
     */
    bool reuse_halation;
} film_develop_params_t;

/** 像素在内存中的通道顺序 */
typedef enum {
    FILM_ORDER_RGB = 0,     /*!< 字节顺序 R、G、B（主机测试、PNG） */
    FILM_ORDER_BGR,         /*!< 字节顺序 B、G、R（ESP32 PPA / JPEG 编解码器的 "RGB888"） */
} film_channel_order_t;

/** 一帧紧凑或带行跨度的 24 位图像 */
typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    size_t stride;      /*!< 每行字节数，至少 width * 3 */
    film_channel_order_t order;
} film_image_t;

esp_err_t film_filter_create(const film_filter_config_t *config, film_filter_handle_t *ret_handle);
esp_err_t film_filter_delete(film_filter_handle_t handle);

/**
 * 原地冲洗一帧图像。
 *
 * @return ESP_OK；参数非法返回 ESP_ERR_INVALID_ARG；图像超过创建时的尺寸返回 ESP_ERR_INVALID_SIZE
 */
esp_err_t film_filter_develop(film_filter_handle_t handle, const film_develop_params_t *params,
                              const film_image_t *image);

/** 胶卷的稳定英文键名（如 "gold"），用于配置文件和日志；编号非法时返回 NULL */
const char *film_filter_get_key(film_id_t film);

/** 由键名查胶卷编号 */
esp_err_t film_filter_find_by_key(const char *key, film_id_t *ret_film);

#ifdef __cplusplus
}
#endif

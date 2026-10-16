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
    FILM_ID_STRAY,      /*!< 野猫 3200：粗粝高反差黑白 */
    FILM_ID_SEPIA,      /*!< 棕褐 100：暖棕调的单色（分离色调实现） */
    FILM_ID_COUNT,
} film_id_t;

typedef struct film_filter_t *film_filter_handle_t;

#define FILM_LOOK_CHANNELS  3

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

/*
 * ---- 分条带冲洗：整幅图像放不进内存时（全分辨率成片），一次只处理其中若干行 ----
 *
 * 结果与整图冲洗逐位一致。流程：
 *
 *   film_filter_strip_begin(整幅尺寸)
 *   if (pre_pass != NONE) {
 *       对每个条带（自上而下或任意顺序，每行恰好一次）：film_filter_strip_pre(条带, 起始行)
 *           IN_PLACE：条带像素已被调色，调用者必须把它写回存放处，第二遍从调色后的像素开始
 *           READ_ONLY：像素不变，只是在累计全图统计（像素风的格子颜色）
 *       film_filter_strip_resolve()
 *   }
 *   对每个条带：film_filter_strip_finish(条带, 起始行)   // 之后条带即为成片像素
 *   film_filter_strip_end()
 *
 * 条带的宽度、通道顺序必须与 begin 一致；同一时刻只能有一个会话，期间不能调用 film_filter_develop。
 * 红晕胶卷的网格行按绝对行号划分，条带起止行不需要对齐。
 */
typedef enum {
    FILM_STRIP_PRE_PASS_NONE = 0,   /*!< 不需要第一遍，直接 finish */
    FILM_STRIP_PRE_PASS_IN_PLACE,   /*!< 第一遍会改写条带（调色），必须写回 */
    FILM_STRIP_PRE_PASS_READ_ONLY,  /*!< 第一遍只读 */
} film_strip_pre_pass_t;

esp_err_t film_filter_strip_begin(film_filter_handle_t handle, const film_develop_params_t *params, uint16_t width,
                                  uint16_t height, film_channel_order_t order);
film_strip_pre_pass_t film_filter_strip_pre_pass(film_filter_handle_t handle);
esp_err_t film_filter_strip_pre(film_filter_handle_t handle, const film_image_t *strip, uint16_t y0);
esp_err_t film_filter_strip_resolve(film_filter_handle_t handle);
esp_err_t film_filter_strip_finish(film_filter_handle_t handle, const film_image_t *strip, uint16_t y0);
void film_filter_strip_end(film_filter_handle_t handle);

/**
 * 取景用的"胶卷外观"：只有色调曲线、饱和度和单色，不含颗粒、红晕、暗角、漏光和像素化。
 * 取景转换把它折进查找表，每像素的运算量与不加外观时相同（见 film_viewfinder.h）。
 * 与成片管线相比是近似：饱和度在曲线之前按色度缩放，暗/亮部色调按曲线输出叠加，单色按亮度取。
 */
typedef struct {
    uint8_t curve[FILM_LOOK_CHANNELS][256];  /*!< 逻辑 R、G、B：输入色阶 → 输出色阶（曝光补偿与暗/亮部色调已折入）；单色时三条相同 */
    int16_t sat_q8;                          /*!< 饱和度，Q8（256 = 1.0）；单色时无意义 */
    bool mono;
} film_look_t;

/**
 * 由胶卷预设和曝光补偿生成取景外观（几十微秒）。没有曲线可言的胶卷（像素风）得到恒等外观。
 * 任何有预设的新胶卷自动获得取景外观，不需要额外登记。
 */
esp_err_t film_filter_look(film_id_t film, float exposure_ev, film_look_t *ret_look);

/** 胶卷的稳定英文键名（如 "gold"），用于配置文件和日志；编号非法时返回 NULL */
const char *film_filter_get_key(film_id_t film);

/** 由键名查胶卷编号 */
esp_err_t film_filter_find_by_key(const char *key, film_id_t *ret_film);

#ifdef __cplusplus
}
#endif

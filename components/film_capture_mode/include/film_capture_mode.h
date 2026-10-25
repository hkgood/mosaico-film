/*
 * film_capture_mode：按相机传感器实际能出的格式，选出取景格式、取景缓冲个数和拍照来源。
 *
 * 纯计算，不碰硬件：设备上由 film_sensor 枚举传感器格式填进来，主机测试直接构造。
 * 规则（详见 README.md）：
 *   - 有 JPEG 拍照通道（OV3640）：取景只为看，选帧率最高、最省内存的 UYVY；拍照用最大的 4:3 JPEG。
 *   - 没有 JPEG 拍照通道（SC101IOT）：快门帧就是取景帧，取景格式决定照片分辨率，
 *     在 PSRAM 放得下的前提下选 4:3 窗口像素最多的，再优先接近 15 fps（暗处曝光时间更长）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 规划时最多看这么多个传感器格式（Kconfig 编进去的一般不到 10 个） */
#define FILM_CAPTURE_MODE_MAX 16

typedef enum {
    FILM_CAPTURE_PIXEL_UYVY,    /*!< YUV422 UYVY：取景转换与暗房小样只认这种 */
    FILM_CAPTURE_PIXEL_JPEG,
    FILM_CAPTURE_PIXEL_OTHER,   /*!< YUYV、RGB565、RAW 等：不用 */
} film_capture_pixel_t;

/** 传感器的一个输出格式 */
typedef struct {
    film_capture_pixel_t pixel;
    uint16_t width;
    uint16_t height;
    uint8_t fps;
} film_capture_mode_t;

/** 规划所需的运行环境 */
typedef struct {
    bool jpeg_still;            /*!< 传感器有可用的 JPEG 拍照通道（需要曝光、白平衡寄存器控制，目前只有 OV3640） */
    size_t psram_free;          /*!< 开流之前的空闲 PSRAM（字节） */
    size_t psram_largest;       /*!< 开流之前最大的连续 PSRAM 块（字节）；每块帧缓冲都要一整块 */
} film_capture_env_t;

typedef struct {
    int preview;                /*!< 取景格式在 modes 里的下标；-1 表示没有合适的，沿用传感器当前格式 */
    uint8_t buffers;            /*!< 取景流的帧缓冲个数（1 或 2） */
    int still;                  /*!< JPEG 拍照格式的下标；-1 表示快门帧就是取景帧 */
    uint16_t photo_width;       /*!< 原片宽（JPEG 尺寸或取景的 4:3 窗口）；两者都没有时为 0 */
    uint16_t photo_height;
} film_capture_plan_t;

/** 16:9 等宽画面中居中的 4:3 窗口尺寸；与 film_viewfinder_window_43 的规则一致（宽取偶数），已是 4:3 或更窄时原样 */
void film_capture_mode_window(uint16_t width, uint16_t height, uint16_t *ret_width, uint16_t *ret_height);

/**
 * 以这个格式开取景流要占的 PSRAM：buffers 块帧缓冲，加 DVP 驱动自己的一块备用缓冲（实测 640×480 双缓冲占 1802 KB）。
 */
size_t film_capture_mode_stream_bytes(const film_capture_mode_t *mode, uint8_t buffers);

/** 选格式；modes 可以为 NULL（count 为 0），这时 ret 里全是"沿用当前格式" */
void film_capture_mode_plan(const film_capture_mode_t *modes, size_t count, const film_capture_env_t *env,
                            film_capture_plan_t *ret);

#ifdef __cplusplus
}
#endif

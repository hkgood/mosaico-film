/*
 * film_exif：生成 JPEG 的 EXIF（APP1）段，并把它拼进已编码的 JPEG。纯 C、无依赖，设备与主机测试共用。
 *
 * 写入的字段：
 *   IFD0   ImageDescription / Make / Model / Orientation / Software / DateTime
 *   Exif   DateTimeOriginal / DateTimeDigitized / OffsetTime* / ExposureBiasValue / UserComment /
 *          PixelXDimension / PixelYDimension
 *
 * 时间策略（严格）：time 小于 FILM_EXIF_TIME_VALID_AFTER（未校时、或设备在 1970 年附近）时，
 * 所有时间标签一律不写，绝不写入虚假的 1970 年。
 *
 * 字节序固定为小端（"II"）。字符串一律按 ASCII 写入，非 ASCII 字节替换为 '?'，
 * 所以需要中文的调用者应先把 JSON 做 \uXXXX 转义。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 早于该时刻（2025-01-01 00:00:00 UTC）的时间视为"设备没校时"，不写入 */
#define FILM_EXIF_TIME_VALID_AFTER 1735689600LL

/** 各字符串字段的长度上限（含结尾 NUL），超出部分被截断 */
#define FILM_EXIF_TEXT_MAX         64
#define FILM_EXIF_DESCRIPTION_MAX  256
#define FILM_EXIF_COMMENT_MAX      1024

/** film_exif_build 输出缓冲的建议大小：覆盖所有字段取最大值也放得下 */
#define FILM_EXIF_SEGMENT_MAX      2048

/** EXIF Orientation 取值（TIFF 6.0 / EXIF 2.32） */
#define FILM_EXIF_ORIENT_NORMAL    1   /*!< 不需要旋转 */
#define FILM_EXIF_ORIENT_ROT180    3   /*!< 显示时旋转 180° */
#define FILM_EXIF_ORIENT_ROT90CW   6   /*!< 显示时顺时针 90° */
#define FILM_EXIF_ORIENT_ROT270CW  8   /*!< 显示时顺时针 270°（即逆时针 90°） */

typedef struct {
    const char *make;           /*!< 厂商，NULL 不写 */
    const char *model;          /*!< 机型，NULL 不写 */
    const char *software;       /*!< 固件/应用版本，NULL 不写 */
    const char *description;    /*!< ImageDescription，NULL 不写 */
    const char *user_comment;   /*!< UserComment（ASCII；约定为 JSON），NULL 不写 */
    /**
     * 拍摄时刻，Unix 秒（UTC）。小于 FILM_EXIF_TIME_VALID_AFTER 时不写任何时间标签。
     * EXIF 的 DateTime 系列是"当地墙钟时间"，本模块写入 time + utc_offset_min。
     */
    int64_t time;
    bool has_utc_offset;        /*!< 为真才写 OffsetTime* 并把墙钟时间换算为当地时间；为假则按 UTC 写、不写偏移 */
    int16_t utc_offset_min;     /*!< 当地时间相对 UTC 的分钟数，如东八区 +480 */
    uint16_t orientation;       /*!< 1..8；0 不写 */
    uint32_t width, height;     /*!< PixelXDimension / PixelYDimension；0 不写 */
    bool has_exposure_bias;
    float exposure_bias_ev;     /*!< 曝光补偿（EV），按 1/100 档写入 */
} film_exif_t;

/**
 * 生成完整的 APP1 段（含 0xFFE1 标记、2 字节长度、"Exif\0\0" 和 TIFF 数据），写入 out。
 * 返回字节数；缓冲不足或参数非法返回 0。out 至少 FILM_EXIF_SEGMENT_MAX 字节可保证成功。
 */
size_t film_exif_build(const film_exif_t *exif, uint8_t *out, size_t capacity);

/**
 * 把 APP1 段拼进 JPEG：紧跟在 SOI 之后，原有的 EXIF 段（若有）被丢弃，其余段与图像数据原样保留。
 * 返回新 JPEG 的字节数；jpeg 不是合法 JPEG、或 out 容量不足返回 0。jpeg 与 out 不能重叠。
 */
size_t film_exif_splice(const uint8_t *jpeg, size_t jpeg_size, const uint8_t *segment, size_t segment_size,
                        uint8_t *out, size_t capacity);

/** 拼接后的大小（用于提前分配缓冲）；jpeg 不合法返回 0 */
size_t film_exif_spliced_size(const uint8_t *jpeg, size_t jpeg_size, size_t segment_size);

/** 从 SOFn 段读出 JPEG 的像素尺寸（不解码）；找不到（遇到 SOS 仍无 SOF、数据截断）返回 false */
bool film_exif_jpeg_size(const uint8_t *jpeg, size_t jpeg_size, uint16_t *ret_width, uint16_t *ret_height);

/** 读出 JPEG 里的 EXIF Orientation；没有或无法解析返回 0。主要供测试与诊断 */
uint16_t film_exif_read_orientation(const uint8_t *jpeg, size_t jpeg_size);

/**
 * 把"握持方向 + 传感器安装方向"换算成 RAW 原片应带的 Orientation（原片是传感器方向、没有转正）。
 * hold_quarter_turns 即 gfx_rot_t 的 0..3。映射由暗房几何推导，host_test/test_film_exif.c 用真实暗房核对。
 */
uint16_t film_exif_orientation_for_raw(int hold_quarter_turns);

#ifdef __cplusplus
}
#endif

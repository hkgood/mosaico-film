/*
 * film_meta：给成片 JPG 与 RAW 原片写 EXIF。
 *
 * 把 film_shot_t（胶卷、曝光补偿、握持方向）整理成 film_exif_t，
 * 再把 APP1 拼进 JPEG，返回交给写盘任务的 blob。
 * 不写任何时间标签（DateTime* / OffsetTime*）。
 * 成片 JPG 已经转正（Orientation 不写）；RAW 是传感器方向，写入 Orientation 让手机里方向正确，
 * 暗房读取 RAW 时完全不看 EXIF，仍按 rot 自己转正，两者互不影响。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "film_port.h"
#include "film_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FILM_META_JPG = 0,   /*!< 成片：已转正 */
    FILM_META_RAW,       /*!< 原片：传感器方向，带 Orientation */
} film_meta_kind_t;

/**
 * 复制 jpeg 并拼入 EXIF，成功返回 blob（由调用者交给写盘任务），失败返回 NULL。
 * 像素尺寸直接从 JPEG 的 SOF 段读取。EXIF 生成失败（极少见）时退化为原样复制，照片不丢。
 */
film_blob_t *film_meta_make_blob(const film_shot_t *shot, film_meta_kind_t kind, const uint8_t *jpeg, size_t size);

#ifdef __cplusplus
}
#endif

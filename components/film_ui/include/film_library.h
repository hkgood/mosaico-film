/*
 * film_library：相册（照片目录 + 索引）。只用 C 标准库文件接口，设备（/nand）与 PC（本地目录）共用。
 *
 * 每张照片在根目录下有四个文件（编号 6 位十进制）：
 *   MF000042.JPG       成片（分享到手机的就是它）
 *   MF000042_RAW.JPG   传感器原片，供"重新冲洗"
 *   MF000042.SCR       屏幕尺寸成片（RGB565，带 film_photo_t 头），大图浏览直接读取
 *   MF000042.THM       缩略图（RGB565，带头），相册网格用
 * INDEX.BIN 保存全部照片的元数据；丢失或损坏时扫描 .SCR 头重建。
 *
 * 线程：句柄由界面线程独占。film_library_write_raw / film_library_path 是无状态函数，
 * 平台的写盘任务可以在任意线程调用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILM_LIBRARY_MAX_PHOTOS 4000
#define FILM_PATH_MAX 96

/** 照片标志位 */
#define FILM_PHOTO_INSTANT    0x01  /*!< 宝丽来相纸（成片含相纸边） */
#define FILM_PHOTO_DATE       0x02  /*!< 已压日期戳 */
#define FILM_PHOTO_REDEVELOP  0x04  /*!< 由重新冲洗得到 */
/** bit 6..5：原片校正的亮度细分（film_balance_t 的 bit 17..16），旧照片为 0 */
#define FILM_PHOTO_BALANCE_FINE_SHIFT 5
#define FILM_PHOTO_BALANCE_FINE_MASK  0x60

/** 一张照片的元数据；同时是 .SCR/.THM 文件头与索引记录，按固定布局写盘（32 字节） */
typedef struct {
    uint32_t id;
    uint32_t jpeg_bytes;    /*!< 成片 JPG 大小，分享页显示总量 */
    int64_t time;           /*!< 拍摄时间（Unix 秒），0 表示未校时 */
    uint16_t roll;
    uint16_t screen_w;
    uint16_t screen_h;
    uint16_t thumb_w;
    uint16_t thumb_h;
    uint8_t film;
    uint8_t flags;
    uint8_t frame;
    uint8_t rot;            /*!< 拍摄时的握持方向（gfx_rot_t） */
    uint16_t balance;       /*!< 冲洗时的原片校正（film_balance.h 编码的低 16 位，细分位在 flags），0 为不校正 */
} film_photo_t;

/** 原片校正细分位在 film_balance_t 中的位置（film_studio.c 断言与 FILM_BALANCE_FINE_SHIFT 一致） */
#define FILM_PHOTO_BALANCE_CODE_FINE_SHIFT 16

/** 从元数据拼回完整的原片校正编码（film_balance_t） */
static inline uint32_t film_photo_balance(const film_photo_t *photo)
{
    const uint32_t fine = ((uint32_t)photo->flags & FILM_PHOTO_BALANCE_FINE_MASK) >> FILM_PHOTO_BALANCE_FINE_SHIFT;
    return photo->balance | (fine << FILM_PHOTO_BALANCE_CODE_FINE_SHIFT);
}

typedef enum {
    FILM_FILE_JPEG = 0,
    FILM_FILE_RAW,
    FILM_FILE_SCREEN,
    FILM_FILE_THUMB,
} film_file_kind_t;

typedef struct film_library_t *film_library_handle_t;

typedef struct {
    const char *root;       /*!< 照片目录，不存在时自动创建 */
} film_library_config_t;

esp_err_t film_library_create(const film_library_config_t *config, film_library_handle_t *ret_handle);
void film_library_delete(film_library_handle_t handle);

size_t film_library_count(film_library_handle_t handle);
/** 第 index 张（0 为最新） */
const film_photo_t *film_library_get(film_library_handle_t handle, size_t index);
/** 按编号查找，ret_index 可为 NULL；找不到返回 NULL */
const film_photo_t *film_library_find(film_library_handle_t handle, uint32_t id, size_t *ret_index);
/** 分配一个新编号（只在内存中递增，照片保存成功后 add） */
uint32_t film_library_reserve_id(film_library_handle_t handle);
esp_err_t film_library_add(film_library_handle_t handle, const film_photo_t *photo);
/** 删除照片及其全部文件 */
esp_err_t film_library_remove(film_library_handle_t handle, uint32_t id);
/**
 * 批量删除照片及其全部文件，索引只写一次。
 * 不在库里的 id（含重复）直接跳过；ret_removed（可为 NULL）返回实际删掉的张数。
 * 返回 ESP_ERR_NOT_FOUND 表示一张都没删；索引写盘失败时返回写盘错误（内存里已删除）。
 */
esp_err_t film_library_remove_many(film_library_handle_t handle, const uint32_t *ids, size_t count,
                                   size_t *ret_removed);
/**
 * 读取 .SCR 或 .THM 像素到 buf（容量 capacity 个像素），返回实际宽高。
 * 文件损坏或尺寸超过容量时返回错误。
 */
esp_err_t film_library_load_pixels(film_library_handle_t handle, uint32_t id, film_file_kind_t kind, uint16_t *buf,
                                   size_t capacity, uint16_t *ret_w, uint16_t *ret_h);

/** 拼出某张照片某个文件的路径 */
void film_library_path(const char *root, uint32_t id, film_file_kind_t kind, char *buf, size_t len);
/** 写一个 .SCR 或 .THM 文件（先写临时文件再改名，断电不会留下半个文件） */
esp_err_t film_library_write_raw(const char *root, const film_photo_t *photo, film_file_kind_t kind,
                                 const uint16_t *pixels, uint16_t width, uint16_t height);

#ifdef __cplusplus
}
#endif

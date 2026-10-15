/*
 * film_pending：待冲洗标记（MF000042.PND）与断电续冲的恢复扫描。
 *
 * 拍摄只负责把原片落盘：RAW 写完之后才写 .PND，所以"有 .PND"就表示 RAW 完整、照片可以续冲。
 * 冲洗成功（JPG/SCR/THM 全部写完）后删掉 .PND。开机时扫描剩下的 .PND，按里面记的拍摄参数
 * 把这些照片重新排进暗房。
 *
 * 标记只有 36 字节、带校验，布局固定（小端），与结构体填充无关：
 *   "MFPD" 版本 标志 握持方向 胶卷 | 编号 | 种子 | 曝光(毫 EV) | 卷 张 | 拍摄时间(8) | CRC32
 * 平台无关，只用 C 标准库；设备与 PC 共用。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FILM_PENDING_BYTES 36

/** 把拍摄参数编码成标记（只编码续冲需要的字段；preview_only / source_id / balance 不属于新拍的照片） */
void film_pending_encode(const film_shot_t *shot, uint8_t out[FILM_PENDING_BYTES]);
/** 解码并校验；长度、魔数、版本、CRC 或取值非法时返回 false，ret 不变 */
bool film_pending_decode(const uint8_t *buf, size_t len, film_shot_t *ret);

/** 读某张照片的 .PND；失败返回 ESP_ERR_NOT_FOUND（缺失）或 ESP_FAIL（损坏） */
esp_err_t film_pending_read(const char *root, uint32_t id, film_shot_t *ret);
/** 删 .PND（不存在也算成功） */
void film_pending_remove(const char *root, uint32_t id);
/** RAW 文件是否完整：以 JPEG SOI 开头、EOI 结尾 */
bool film_pending_raw_complete(const char *root, uint32_t id);

/**
 * 开机恢复：
 *   - 收集合格的待冲洗照片（有 .PND、标记有效、RAW 完整）的拍摄参数，按编号升序写入 shots；
 *   - 损坏的 .PND 或 RAW 不完整的（连同它的文件）直接删掉；
 *   - 比所有 .SCR 都新、既没有 .PND 的 _RAW.JPG 是写到一半断电的孤儿，删掉。
 * 返回写入的张数（最多 cap）。
 */
size_t film_pending_recover(const char *root, film_shot_t *shots, size_t cap);

#ifdef __cplusplus
}
#endif

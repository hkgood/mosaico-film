/*
 * film_settings：持久化的用户设置。界面（film_app）与启动代码（main，开机动画之前就要读）共用同一份结构与读取规则。
 *
 * 结构只往末尾追加字段；读取时按"当前大小 → 历史大小"依次尝试，旧版本保存的短数据新字段取 0（即默认值），
 * 所以升级不会丢卷号、张数等已有设置。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FILM_SETTINGS_VERSION       1
#define FILM_SETTINGS_ROLL_FRAMES   36      /*!< 一卷张数，与界面的 ROLL_FRAMES 一致 */

/** "无操作多久关屏"的档位数；调暗在关屏时长的一半 */
#define FILM_SCREEN_TIMEOUT_LEVELS  4

typedef struct {
    uint8_t version;
    uint8_t film;
    uint8_t instant;
    uint8_t date_stamp;
    uint16_t roll;
    uint8_t frame;          /*!< 本卷已拍张数 0..36 */
    uint8_t crop_43;        /*!< M6 取景范围：1 4:3 横幅裁切（默认，与 1.0.0 一致）、0 全画幅（3:4 竖幅） */
    /* ---- 以下是后加的字段，旧数据里不存在，读到时为 0 ---- */
    uint8_t film_look;      /*!< 胶卷取景：0 关（默认）、1 取景带胶卷色调 */
    uint8_t skip_boot;      /*!< 跳过开机动画：0 播放（默认）、1 跳过 */
    uint8_t sound_off;      /*!< 关闭声音（快门、拨盘、开机）：0 开（默认）、1 关 */
    uint8_t haptic_off;     /*!< 关闭振动：0 开（默认）、1 关 */
    uint8_t screen_timeout; /*!< 关屏时长档位 0..FILM_SCREEN_TIMEOUT_LEVELS-1，0 为默认 30 秒 */
    uint8_t framing_ver;    /*!< 取景范围字段的含义版本：0 = 旧数据（crop_43 为 0 只是"没设过"），1 = 用户可选 */
    uint8_t flash_off;      /*!< 关闭自动闪光（暗光时拍照补光）：0 开（默认）、1 关 */
} film_settings_t;

/** 取出已保存数据的函数（与 film_port_t.settings_load 同形）：长度与保存时不一致返回 false */
typedef bool (*film_settings_load_fn)(void *ctx, void *data, size_t len);

/** 出厂默认 */
void film_settings_defaults(film_settings_t *out);

/**
 * 读取并校验已保存的设置：先按当前大小，再依次退回历史大小。
 * 没有数据或内容不合法时返回 false，out 为出厂默认。
 */
bool film_settings_read(film_settings_load_fn load, void *ctx, film_settings_t *out);

/** 关屏时长档位 → 毫秒：无操作多久调暗、多久关屏 */
void film_settings_screen_timeout_ms(uint8_t level, uint32_t *ret_dim_ms, uint32_t *ret_off_ms);

#ifdef __cplusplus
}
#endif

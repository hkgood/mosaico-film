#include "film_settings.h"

#include <string.h>

#include "film_filter.h"

/* 历史版本保存的大小：加 film_look 之前 8 字节；加 film_look 与一个保留字节后 10 字节 */
static const size_t k_history_sizes[] = {
    sizeof(film_settings_t),
    offsetof(film_settings_t, sound_off),
    offsetof(film_settings_t, film_look),
};

/* 关屏时长（毫秒）：档位 0 与旧版本固定值一致（15 秒调暗、30 秒关屏） */
static const uint32_t k_off_ms[FILM_SCREEN_TIMEOUT_LEVELS] = { 30000, 60000, 120000, 300000 };

void film_settings_defaults(film_settings_t *out)
{
    *out = (film_settings_t) {
        .version = FILM_SETTINGS_VERSION,
        .film = FILM_ID_GOLD,
        .instant = 0,
        .date_stamp = 1,
        .roll = 1,
        .frame = 0,
    };
}

bool film_settings_read(film_settings_load_fn load, void *ctx, film_settings_t *out)
{
    film_settings_defaults(out);
    if (!load) {
        return false;
    }
    for (size_t i = 0; i < sizeof(k_history_sizes) / sizeof(k_history_sizes[0]); ++i) {
        film_settings_t s;
        memset(&s, 0, sizeof(s));
        if (!load(ctx, &s, k_history_sizes[i])) {
            continue;
        }
        if (s.version != FILM_SETTINGS_VERSION || s.film >= FILM_ID_COUNT || s.frame > FILM_SETTINGS_ROLL_FRAMES ||
            s.roll == 0) {
            return false;   /* 内容坏了：用默认，不再往更短的大小试 */
        }
        /* 档位越界（数据损坏或未来版本）按默认处理，其余开关按非零即开 */
        if (s.screen_timeout >= FILM_SCREEN_TIMEOUT_LEVELS) {
            s.screen_timeout = 0;
        }
        *out = s;
        return true;
    }
    return false;
}

void film_settings_screen_timeout_ms(uint8_t level, uint32_t *ret_dim_ms, uint32_t *ret_off_ms)
{
    const uint32_t off = k_off_ms[level < FILM_SCREEN_TIMEOUT_LEVELS ? level : 0];
    *ret_off_ms = off;
    *ret_dim_ms = off / 2;
}

/*
 * 8 卷胶卷的调色参数。数值来自已确认的设计原型 films.py，修改时两边需保持一致，
 * 并用主机端对比脚本重新验证。
 */
#include <string.h>

#include "film_filter_private.h"

/* 曲线控制点写法：CURVE5(输入0,输出0, 输入1,输出1, ...) */
#define CURVE5(x0, y0, x1, y1, x2, y2, x3, y3, x4, y4) \
    { .count = 5, .points = { { x0, y0 }, { x1, y1 }, { x2, y2 }, { x3, y3 }, { x4, y4 } } }
#define CURVE_IDENTITY  { .count = 2, .points = { { 0, 0 }, { 255, 255 } } }

static const film_preset_t s_presets[FILM_ID_COUNT] = {
    [FILM_ID_GOLD] = {
        .key = "gold",
        .curve = {
            CURVE5(0, 8, 64, 72, 128, 142, 192, 206, 255, 252),
            CURVE5(0, 4, 64, 64, 128, 132, 192, 196, 255, 245),
            CURVE5(0, 10, 64, 56, 128, 112, 192, 170, 255, 222),
        },
        .saturation = 1.12f, .vignette = 0.28f, .grain = 0.03f,
    },
    [FILM_ID_PORTRA] = {
        .key = "portra",
        .curve = {
            CURVE5(0, 22, 64, 76, 128, 138, 192, 198, 255, 246),
            CURVE5(0, 18, 64, 70, 128, 130, 192, 190, 255, 242),
            CURVE5(0, 20, 64, 66, 128, 122, 192, 180, 255, 232),
        },
        .saturation = 0.9f, .shadow_tint = { 4, 0, 6 }, .vignette = 0.14f, .grain = 0.025f,
    },
    [FILM_ID_GREEN] = {
        .key = "green",
        .curve = {
            CURVE5(0, 6, 64, 58, 128, 124, 192, 190, 255, 248),
            CURVE5(0, 12, 64, 70, 128, 136, 192, 198, 255, 250),
            CURVE5(0, 24, 64, 74, 128, 130, 192, 186, 255, 236),
        },
        .saturation = 1.0f, .shadow_tint = { -6, 6, 10 }, .vignette = 0.2f, .grain = 0.03f,
    },
    [FILM_ID_CROSS] = {
        .key = "cross",
        .curve = {
            CURVE5(0, 0, 64, 40, 128, 136, 192, 222, 255, 255),
            /* 绿略低于红、蓝的高光不压太狠：高光偏暖黄而不是黄绿，保留蓝色暗部与高对比 */
            CURVE5(0, 0, 64, 48, 128, 136, 192, 212, 255, 248),
            CURVE5(0, 42, 64, 72, 128, 122, 192, 166, 255, 204),
        },
        .saturation = 1.28f, .vignette = 0.6f, .grain = 0.04f,
    },
    [FILM_ID_BW] = {
        .key = "bw",
        .curve = {
            CURVE5(0, 6, 64, 48, 128, 132, 192, 212, 255, 250),
            CURVE_IDENTITY,
            CURVE_IDENTITY,
        },
        .saturation = 1.0f, .mono = true, .vignette = 0.32f, .grain = 0.075f,
    },
    [FILM_ID_FADED] = {
        .key = "faded",
        .curve = {
            CURVE5(0, 48, 64, 92, 128, 150, 192, 204, 255, 236),
            CURVE5(0, 36, 64, 76, 128, 128, 192, 182, 255, 220),
            CURVE5(0, 44, 64, 70, 128, 110, 192, 156, 255, 196),
        },
        .saturation = 0.72f, .vignette = 0.22f, .grain = 0.05f,
    },
    [FILM_ID_NIGHT] = {
        .key = "night",
        .curve = {
            CURVE5(0, 8, 64, 54, 128, 118, 192, 186, 255, 246),
            CURVE5(0, 10, 64, 62, 128, 128, 192, 196, 255, 250),
            CURVE5(0, 24, 64, 84, 128, 152, 192, 214, 255, 255),
        },
        .saturation = 1.05f, .halation = 0.55f, .vignette = 0.3f, .grain = 0.06f,
    },
    [FILM_ID_PIXEL] = {
        .key = "pixel",
        .curve = { CURVE_IDENTITY, CURVE_IDENTITY, CURVE_IDENTITY },
        .saturation = 1.0f, .pixel = true,
    },
};

const film_preset_t *film_preset_get(film_id_t film)
{
    if ((unsigned)film >= FILM_ID_COUNT) {
        return NULL;
    }
    return &s_presets[film];
}

const char *film_filter_get_key(film_id_t film)
{
    const film_preset_t *preset = film_preset_get(film);
    return preset ? preset->key : NULL;
}

esp_err_t film_filter_find_by_key(const char *key, film_id_t *ret_film)
{
    if (key == NULL || ret_film == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < FILM_ID_COUNT; i++) {
        if (strcmp(s_presets[i].key, key) == 0) {
            *ret_film = (film_id_t)i;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

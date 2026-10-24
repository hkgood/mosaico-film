/*
 * 持久化设置：当前大小往返、旧版本短数据（新字段取默认）、损坏数据退回出厂默认、关屏时长档位表。
 *
 *   film_settings_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_settings.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* 假的存储：只在大小与保存时一致才交出数据，和设备上的 NVS blob 行为相同 */
typedef struct {
    uint8_t data[64];
    size_t size;
} store_t;

static bool store_load(void *ctx, void *data, size_t len)
{
    const store_t *s = ctx;
    if (s->size == 0 || s->size != len) {
        return false;
    }
    memcpy(data, s->data, len);
    return true;
}

static void store_put(store_t *s, const void *data, size_t size)
{
    memset(s, 0, sizeof(*s));
    memcpy(s->data, data, size);
    s->size = size;
}

static void test_defaults_and_empty(void)
{
    film_settings_t s;
    store_t empty = { .size = 0 };
    CHECK(!film_settings_read(store_load, &empty, &s));
    CHECK(s.version == FILM_SETTINGS_VERSION && s.roll == 1 && s.frame == 0 && s.date_stamp == 1);
    CHECK(s.skip_boot == 0 && s.sound_off == 0 && s.haptic_off == 0 && s.screen_timeout == 0);
    CHECK(s.flash_off == 0);   /* 自动闪光默认开 */
    CHECK(!film_settings_read(NULL, NULL, &s));
}

static void test_roundtrip(void)
{
    film_settings_t in;
    film_settings_defaults(&in);
    in.film = 3;
    in.roll = 12;
    in.frame = 20;
    in.crop_43 = 1;
    in.film_look = 1;
    in.skip_boot = 1;
    in.sound_off = 1;
    in.haptic_off = 1;
    in.screen_timeout = 2;
    in.flash_off = 1;
    store_t st;
    store_put(&st, &in, sizeof(in));
    film_settings_t out;
    CHECK(film_settings_read(store_load, &st, &out));
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);
}

/* 升级前保存的 8 字节、10 字节数据：原有字段保留，新增的开关全是默认（0） */
static void test_legacy_sizes(void)
{
    CHECK(offsetof(film_settings_t, film_look) == 8);   /* 8 字节版本的大小 */
    CHECK(offsetof(film_settings_t, sound_off) == 10);  /* 10 字节版本的大小 */
    film_settings_t in;
    film_settings_defaults(&in);
    in.film = 5;
    in.instant = 1;
    in.roll = 7;
    in.frame = 9;
    in.crop_43 = 1;
    in.film_look = 1;
    const size_t sizes[] = { 8, 10 };
    for (size_t i = 0; i < 2; ++i) {
        store_t st;
        store_put(&st, &in, sizes[i]);
        film_settings_t out;
        CHECK(film_settings_read(store_load, &st, &out));
        CHECK(out.film == 5 && out.instant == 1 && out.roll == 7 && out.frame == 9 && out.crop_43 == 1);
        CHECK(out.film_look == (sizes[i] > 8 ? 1 : 0));
        CHECK(out.skip_boot == 0 && out.sound_off == 0 && out.haptic_off == 0 && out.screen_timeout == 0);
        CHECK(out.flash_off == 0);
    }
}

/* 加自动闪光之前保存的 14 字节数据：原有设置全部保留，自动闪光为默认的开 */
static void test_pre_flash_size(void)
{
    CHECK(offsetof(film_settings_t, flash_off) == 14);
    film_settings_t in;
    film_settings_defaults(&in);
    in.roll = 3;
    in.frame = 17;
    in.sound_off = 1;
    in.screen_timeout = 3;
    in.flash_off = 1;   /* 不在 14 字节里，读出来应是默认 */
    store_t st;
    store_put(&st, &in, 14);
    film_settings_t out;
    CHECK(film_settings_read(store_load, &st, &out));
    CHECK(out.roll == 3 && out.frame == 17 && out.sound_off == 1 && out.screen_timeout == 3);
    CHECK(out.flash_off == 0);
}

static void test_corrupt(void)
{
    film_settings_t in;
    film_settings_defaults(&in);
    film_settings_t out;
    store_t st;

    in.version = 99;
    store_put(&st, &in, sizeof(in));
    CHECK(!film_settings_read(store_load, &st, &out));
    CHECK(out.version == FILM_SETTINGS_VERSION && out.roll == 1);

    film_settings_defaults(&in);
    in.frame = FILM_SETTINGS_ROLL_FRAMES + 1;
    store_put(&st, &in, sizeof(in));
    CHECK(!film_settings_read(store_load, &st, &out));

    film_settings_defaults(&in);
    in.roll = 0;
    store_put(&st, &in, sizeof(in));
    CHECK(!film_settings_read(store_load, &st, &out));

    film_settings_defaults(&in);
    in.film = 200;
    store_put(&st, &in, sizeof(in));
    CHECK(!film_settings_read(store_load, &st, &out));

    /* 不认识的大小：当作没有数据 */
    film_settings_defaults(&in);
    store_put(&st, &in, 11);
    CHECK(!film_settings_read(store_load, &st, &out));

    /* 档位越界只重置这一项，其余设置保留 */
    film_settings_defaults(&in);
    in.roll = 4;
    in.screen_timeout = 9;
    store_put(&st, &in, sizeof(in));
    CHECK(film_settings_read(store_load, &st, &out));
    CHECK(out.roll == 4 && out.screen_timeout == 0);
}

/* 取景范围默认 4:3；旧数据（没有 framing_ver）的 crop_43=0 只是"没设过"，升级成 4:3；用户之后选的全画幅会保留 */
static void test_framing_default_and_migration(void)
{
    film_settings_t d;
    film_settings_defaults(&d);
    CHECK(d.crop_43 == 1 && d.framing_ver == 1);

    film_settings_t old = d;
    old.crop_43 = 0;
    old.framing_ver = 0;
    store_t st;
    film_settings_t out;
    store_put(&st, &old, sizeof(old));
    CHECK(film_settings_read(store_load, &st, &out) && out.crop_43 == 1 && out.framing_ver == 1);
    store_put(&st, &old, 8);
    CHECK(film_settings_read(store_load, &st, &out) && out.crop_43 == 1);

    film_settings_t chosen = d;
    chosen.crop_43 = 0;     /* 用户在机身选择里切到全画幅 */
    store_put(&st, &chosen, sizeof(chosen));
    CHECK(film_settings_read(store_load, &st, &out) && out.crop_43 == 0);
}

static void test_screen_timeout(void)
{
    uint32_t dim, off;
    film_settings_screen_timeout_ms(0, &dim, &off);
    CHECK(dim == 15000 && off == 30000);   /* 与旧版本固定值一致 */
    uint32_t prev = 0;
    for (uint8_t level = 0; level < FILM_SCREEN_TIMEOUT_LEVELS; ++level) {
        film_settings_screen_timeout_ms(level, &dim, &off);
        CHECK(off > prev && dim > 0 && dim < off);
        prev = off;
    }
    film_settings_screen_timeout_ms(FILM_SCREEN_TIMEOUT_LEVELS, &dim, &off);   /* 越界回到默认档 */
    CHECK(off == 30000);
}

int main(void)
{
    test_defaults_and_empty();
    test_roundtrip();
    test_legacy_sizes();
    test_pre_flash_size();
    test_corrupt();
    test_framing_default_and_migration();
    test_screen_timeout();
    printf("film_settings_test: ok\n");
    return 0;
}

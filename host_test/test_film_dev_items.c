/*
 * 相册里"还在冲洗"的照片（app_dev.c）：排队 → 小样缩略图 → 进度 → 入册后消失。
 * 不需要完整界面：直接构造 film_app_t，只喂平台事件。
 *
 *   film_dev_items_test
 */
#include <stdio.h>
#include <stdlib.h>

#include "app_private.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static film_app_t s_app;

static void send(film_event_type_t type, uint32_t id, uint16_t progress)
{
    film_event_t ev = { .type = type, .photo_id = id, .progress = progress };
    ev.meta.id = id;
    app_dev_event(&s_app, &ev);
}

int main(void)
{
    uint16_t *pool = calloc((size_t)DEV_ITEMS_MAX * THUMB_MAX_W * THUMB_MAX_H, sizeof(uint16_t));
    CHECK(pool);
    app_dev_bind_pixels(&s_app, pool);

    /* 拍下三张：新的在前，都在排队 */
    send(FILM_EVT_SHUTTER_DONE, 10, 0);
    send(FILM_EVT_SHUTTER_DONE, 11, 0);
    send(FILM_EVT_SHUTTER_DONE, 12, 0);
    CHECK(s_app.n_dev == 3);
    CHECK(app_dev_get(&s_app, 0)->id == 12 && app_dev_get(&s_app, 2)->id == 10);
    CHECK(app_dev_get(&s_app, 0)->progress == 0 && app_dev_get(&s_app, 3) == NULL);
    send(FILM_EVT_SHUTTER_DONE, 11, 0);          /* 重复事件不重复建项 */
    CHECK(s_app.n_dev == 3);

    /* 小样：480x360 的纯色屏幕图缩 3 倍成 160x120，颜色不变 */
    uint16_t *screen = malloc(480 * 360 * sizeof(uint16_t));
    CHECK(screen);
    for (int i = 0; i < 480 * 360; ++i) {
        screen[i] = 0xF81F;
    }
    film_event_t dev = { .type = FILM_EVT_DEVELOPED, .photo_id = 11, .screen = screen, .width = 480, .height = 360 };
    app_dev_event(&s_app, &dev);
    const dev_item_t *it = app_dev_get(&s_app, 1);
    CHECK(it->id == 11 && it->thumb.w == 160 && it->thumb.h == 120);
    CHECK(it->thumb.pixels[0] == 0xF81F && it->thumb.pixels[160 * 120 - 1] == 0xF81F);
    CHECK(app_dev_get(&s_app, 0)->thumb.w == 0);  /* 别的项不受影响 */

    /* 预览（重新冲洗）与不认识的编号不碰列表 */
    dev.preview = true;
    dev.photo_id = 10;
    app_dev_event(&s_app, &dev);
    CHECK(app_dev_get(&s_app, 2)->thumb.w == 0);
    send(FILM_EVT_PROGRESS, 99, 500);
    send(FILM_EVT_SHUTTER_DONE, 0, 0);
    CHECK(s_app.n_dev == 4);                     /* 编号 0 也是一项，只是没人会发 */
    send(FILM_EVT_SAVED, 0, 0);
    CHECK(s_app.n_dev == 3);

    /* 进度只增不减，封顶 */
    send(FILM_EVT_PROGRESS, 10, 300);
    send(FILM_EVT_PROGRESS, 10, 200);
    CHECK(app_dev_get(&s_app, 2)->progress == 300);
    send(FILM_EVT_PROGRESS, 10, 5000);
    CHECK(app_dev_get(&s_app, 2)->progress == FILM_PROGRESS_DONE);

    /* 入册：移除后其余项连同小样一起前移 */
    send(FILM_EVT_SAVED, 10, 0);
    CHECK(s_app.n_dev == 2);
    CHECK(app_dev_get(&s_app, 0)->id == 12 && app_dev_get(&s_app, 1)->id == 11);
    CHECK(app_dev_get(&s_app, 1)->thumb.w == 160 && app_dev_get(&s_app, 1)->thumb.pixels[5] == 0xF81F);

    /* 失败：移除；列表满了不再收新的；被移除项的缓冲可以复用且不带旧小样 */
    send(FILM_EVT_SHOT_FAILED, 12, 0);
    CHECK(s_app.n_dev == 1 && app_dev_get(&s_app, 0)->id == 11);
    for (uint32_t id = 20; id < 30; ++id) {
        send(FILM_EVT_SHUTTER_DONE, id, 0);
    }
    CHECK(s_app.n_dev == DEV_ITEMS_MAX);
    for (size_t k = 0; k < s_app.n_dev; ++k) {
        for (size_t j = k + 1; j < s_app.n_dev; ++j) {
            CHECK(app_dev_get(&s_app, k)->thumb.pixels != app_dev_get(&s_app, j)->thumb.pixels);
        }
    }
    CHECK(app_dev_get(&s_app, 3)->id == 11 && app_dev_get(&s_app, 0)->thumb.w == 0);

    free(screen);
    free(pool);
    puts("film_dev_items_test: ok");
    return 0;
}

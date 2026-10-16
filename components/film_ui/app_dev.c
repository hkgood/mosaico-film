/*
 * 相册里"还在冲洗"的照片：从快门到文件写完之间，照片还没进相册（SAVED 才入册），
 * 这里在内存里跟踪它们，让相册能显示小样缩略图、排队状态和冲洗进度。
 *
 *   SHUTTER_DONE → 建一项（排队中）     DEVELOPED(小样) → 缩成缩略图
 *   PROGRESS     → 更新进度             SAVED / SHOT_FAILED → 移除
 *
 * 只处理拍摄产生的事件：重新冲洗的预览（preview）与没有 SHUTTER_DONE 的编号一律忽略。
 */
#include <string.h>

#include "app_private.h"

static dev_item_t *find(film_app_t *app, uint32_t id)
{
    for (size_t i = 0; i < app->n_dev; ++i) {
        if (app->dev_items[i].id == id) {
            return &app->dev_items[i];
        }
    }
    return NULL;
}

/** 移除一项：后面的前移，像素缓冲指针跟着结构体走，被移除项的缓冲换到末尾复用 */
static void remove_item(film_app_t *app, dev_item_t *item)
{
    const size_t pos = (size_t)(item - app->dev_items);
    const dev_item_t gone = *item;
    memmove(item, item + 1, (app->n_dev - pos - 1) * sizeof(*item));
    --app->n_dev;
    dev_item_t *tail = &app->dev_items[app->n_dev];
    memset(tail, 0, sizeof(*tail));
    tail->thumb.pixels = gone.thumb.pixels;
}

/**
 * 把 w×h 的屏幕图按整数倍盒式平均缩进 THUMB_MAX 以内（RGB565 逐通道平均）。
 * 480 宽的小样缩 3 倍得到 160 宽，与相册里从文件读出的缩略图同尺寸。
 */
static void shrink_to_thumb(thumb_slot_t *dst, const uint16_t *src, int w, int h)
{
    int f = 1;
    while (w / f > THUMB_MAX_W || h / f > THUMB_MAX_H) {
        ++f;
    }
    const int tw = w / f, th = h / f;
    const int area = f * f;
    for (int y = 0; y < th; ++y) {
        for (int x = 0; x < tw; ++x) {
            int r = 0, g = 0, b = 0;
            for (int j = 0; j < f; ++j) {
                const uint16_t *row = src + (size_t)(y * f + j) * (size_t)w + (size_t)(x * f);
                for (int i = 0; i < f; ++i) {
                    const uint16_t p = row[i];
                    r += (p >> 11) & 0x1F;
                    g += (p >> 5) & 0x3F;
                    b += p & 0x1F;
                }
            }
            dst->pixels[y * tw + x] = (uint16_t)(((r / area) << 11) | ((g / area) << 5) | (b / area));
        }
    }
    dst->w = (uint16_t)tw;
    dst->h = (uint16_t)th;
}

void app_dev_event(film_app_t *app, const film_event_t *ev)
{
    if (ev->preview) {
        return;
    }
    switch (ev->type) {
    case FILM_EVT_SHUTTER_DONE: {
        if (find(app, ev->photo_id) || app->n_dev >= DEV_ITEMS_MAX) {
            return;
        }
        dev_item_t *item = &app->dev_items[app->n_dev++];
        uint16_t *pixels = item->thumb.pixels;
        memset(item, 0, sizeof(*item));
        item->thumb.pixels = pixels;
        item->id = ev->photo_id;
        item->instant = app->settings.instant;   /* 快门刚按下，当前机身就是拍这张的机身 */
        break;
    }
    case FILM_EVT_DEVELOPED: {
        dev_item_t *it = find(app, ev->photo_id);
        if (it && ev->screen && ev->width && ev->height && it->thumb.pixels) {
            shrink_to_thumb(&it->thumb, ev->screen, ev->width, ev->height);
        }
        break;
    }
    case FILM_EVT_PROGRESS: {
        dev_item_t *it = find(app, ev->photo_id);
        if (it && ev->progress > it->progress) {
            it->progress = ev->progress > FILM_PROGRESS_DONE ? FILM_PROGRESS_DONE : ev->progress;
        }
        break;
    }
    case FILM_EVT_SAVED:
    case FILM_EVT_SHOT_FAILED: {
        dev_item_t *it = find(app, ev->type == FILM_EVT_SAVED ? ev->meta.id : ev->photo_id);
        if (it) {
            remove_item(app, it);
        }
        break;
    }
    default:
        break;
    }
}

const dev_item_t *app_dev_get(const film_app_t *app, size_t k)
{
    return k < app->n_dev ? &app->dev_items[app->n_dev - 1 - k] : NULL;
}

void app_dev_bind_pixels(film_app_t *app, uint16_t *pool)
{
    for (size_t i = 0; i < DEV_ITEMS_MAX; ++i) {
        app->dev_items[i].thumb.pixels = pool + i * THUMB_MAX_W * THUMB_MAX_H;
    }
}

#include "film_capture_mode.h"

#include <string.h>

/* 取景帧最大 480×360（film_port.h 的 FILM_VF_M6_*）：4:3 窗口至少要这么大，取景才不放大 */
#define PREVIEW_MIN_WIDTH       480
#define PREVIEW_MIN_HEIGHT      360
/* 低于这个帧率的格式不拿来取景（OV3640 1024×768 的 5 fps 档之类） */
#define PREVIEW_MIN_FPS         10
/* 快门帧当原片时首选的帧率：帧率越低单帧最长曝光越长，暗处噪点越少；15 fps 时取景转换也跟得上 */
#define FRAME_STILL_FPS         15
/* DVP 驱动在应用的帧缓冲之外还会自己再申请一块同样大小的备用缓冲 */
#define DRIVER_BACKUP_BUFFERS   1
/* 取景开着时还要留给界面、相册与分享的 PSRAM */
#define PREVIEW_RESERVE_BYTES   (1024u * 1024u)
#define PREVIEW_MAX_BUFFERS     2
#define BYTES_PER_UYVY_PIXEL    2u

void film_capture_mode_window(uint16_t width, uint16_t height, uint16_t *ret_width, uint16_t *ret_height)
{
    uint32_t w = width;
    if ((uint32_t)width * 3u > (uint32_t)height * 4u) {
        w = ((uint32_t)height * 4u / 3u) & ~1u;
    }
    *ret_width = (uint16_t)w;
    *ret_height = height;
}

static size_t frame_bytes(const film_capture_mode_t *mode)
{
    return (size_t)mode->width * mode->height * BYTES_PER_UYVY_PIXEL;
}

size_t film_capture_mode_stream_bytes(const film_capture_mode_t *mode, uint8_t buffers)
{
    return frame_bytes(mode) * (size_t)(buffers + DRIVER_BACKUP_BUFFERS);
}

static uint32_t window_pixels(const film_capture_mode_t *mode)
{
    uint16_t w, h;
    film_capture_mode_window(mode->width, mode->height, &w, &h);
    return (uint32_t)w * h;
}

/** 能不能拿来取景：UYVY、横幅（暗房按横幅传感器设计）、4:3 窗口够大、帧率够 */
static bool preview_usable(const film_capture_mode_t *mode)
{
    uint16_t w, h;
    film_capture_mode_window(mode->width, mode->height, &w, &h);
    return mode->pixel == FILM_CAPTURE_PIXEL_UYVY && mode->width >= mode->height && w >= PREVIEW_MIN_WIDTH &&
           h >= PREVIEW_MIN_HEIGHT && mode->fps >= PREVIEW_MIN_FPS;
}

/**
 * 开流后 PSRAM 还够不够：流本身，加拍照时从流里复制出的 4:3 窗口（小样，或无 JPEG 传感器的原片），
 * 再加界面余量。每块帧缓冲还要有一整块连续空间。
 */
static bool fits(const film_capture_mode_t *mode, uint8_t buffers, const film_capture_env_t *env)
{
    const size_t window_bytes = (size_t)window_pixels(mode) * BYTES_PER_UYVY_PIXEL;
    const size_t need = film_capture_mode_stream_bytes(mode, buffers) + window_bytes + PREVIEW_RESERVE_BYTES;
    return need <= env->psram_free && frame_bytes(mode) <= env->psram_largest;
}

static unsigned fps_distance(uint8_t fps, uint8_t target)
{
    return fps > target ? (unsigned)(fps - target) : (unsigned)(target - fps);
}

/** a 是否比 b 更适合做 JPEG 传感器的取景：帧率高优先，再取小的（省内存、省转换） */
static bool better_for_jpeg(const film_capture_mode_t *a, const film_capture_mode_t *b)
{
    if (a->fps != b->fps) {
        return a->fps > b->fps;
    }
    return frame_bytes(a) < frame_bytes(b);
}

/** a 是否比 b 更适合做快门帧：窗口像素多优先，再取接近 FRAME_STILL_FPS 的，再取整帧小的（同窗口时省内存） */
static bool better_for_frame(const film_capture_mode_t *a, const film_capture_mode_t *b)
{
    const uint32_t pa = window_pixels(a), pb = window_pixels(b);
    if (pa != pb) {
        return pa > pb;
    }
    const unsigned da = fps_distance(a->fps, FRAME_STILL_FPS), db = fps_distance(b->fps, FRAME_STILL_FPS);
    if (da != db) {
        return da < db;
    }
    return frame_bytes(a) < frame_bytes(b);
}

/** 最大的 4:3 JPEG 格式，没有返回 -1 */
static int pick_jpeg(const film_capture_mode_t *modes, size_t count)
{
    int best = -1;
    for (size_t i = 0; i < count; ++i) {
        const film_capture_mode_t *m = &modes[i];
        if (m->pixel != FILM_CAPTURE_PIXEL_JPEG || (uint32_t)m->width * 3u != (uint32_t)m->height * 4u) {
            continue;
        }
        if (best < 0 || (uint32_t)m->width * m->height > (uint32_t)modes[best].width * modes[best].height) {
            best = (int)i;
        }
    }
    return best;
}

void film_capture_mode_plan(const film_capture_mode_t *modes, size_t count, const film_capture_env_t *env,
                            film_capture_plan_t *ret)
{
    memset(ret, 0, sizeof(*ret));
    ret->preview = -1;
    ret->still = -1;
    ret->buffers = PREVIEW_MAX_BUFFERS;
    if (!modes || !env) {
        return;
    }
    if (count > FILM_CAPTURE_MODE_MAX) {
        count = FILM_CAPTURE_MODE_MAX;
    }
    const int jpeg = env->jpeg_still ? pick_jpeg(modes, count) : -1;

    /* 按偏好排出候选顺序（插入排序，最多 FILM_CAPTURE_MODE_MAX 个） */
    int order[FILM_CAPTURE_MODE_MAX];
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!preview_usable(&modes[i])) {
            continue;
        }
        size_t pos = n++;
        while (pos > 0) {
            const film_capture_mode_t *prev = &modes[order[pos - 1]];
            const bool better = jpeg >= 0 ? better_for_jpeg(&modes[i], prev) : better_for_frame(&modes[i], prev);
            if (!better) {
                break;
            }
            order[pos] = order[pos - 1];
            --pos;
        }
        order[pos] = (int)i;
    }

    /* 依次试：同一格式先试双缓冲（取景更流畅），放不下再试单缓冲 */
    for (size_t k = 0; k < n && ret->preview < 0; ++k) {
        for (uint8_t buffers = PREVIEW_MAX_BUFFERS; buffers >= 1; --buffers) {
            if (fits(&modes[order[k]], buffers, env)) {
                ret->preview = order[k];
                ret->buffers = buffers;
                break;
            }
        }
    }

    if (jpeg >= 0) {
        ret->still = jpeg;
        ret->photo_width = modes[jpeg].width;
        ret->photo_height = modes[jpeg].height;
    } else if (ret->preview >= 0) {
        film_capture_mode_window(modes[ret->preview].width, modes[ret->preview].height, &ret->photo_width,
                                 &ret->photo_height);
    }
}

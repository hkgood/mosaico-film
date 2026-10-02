#include "film_shell.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/** 界面推进周期：约 60 Hz 的检查，实际重绘只在画面变化时发生 */
#define SHELL_TICK_MS 16
/** 远程触摸队列深度（2 的幂）：Gateway 一个手势按 40 ms 一点送达，一帧最多来一两个 */
#define REMOTE_QUEUE_LEN 16u
/** 实体按键队列深度（2 的幂）：按下 + 松开一对，一帧内连按几次也装得下 */
#define KEY_QUEUE_LEN 8u
/** 绘制耗时统计周期 */
#define STATS_PERIOD_MS 5000u

typedef struct {
    int16_t x, y;
    bool pressed;
} remote_sample_t;

typedef struct {
    uint8_t key;        /*!< film_key_t */
    bool pressed;
} key_sample_t;

struct film_shell_t {
    film_shell_config_t config;
    film_app_handle_t app;      /*!< 开机动画期间为 NULL */
    uint16_t *frame;            /*!< 当前完整画面（开机动画或 film_app） */
    void *timer;
    uint32_t intro_t0;
    bool intro_playing;
    bool dirty;
    /* 单生产者/单消费者环形队列：tail 只由生产者写，head 只由渲染任务写 */
    remote_sample_t remote[REMOTE_QUEUE_LEN];
    atomic_uint remote_head;
    atomic_uint remote_tail;
    /* 实体按键队列，同样是单生产者/单消费者（生产者是按键驱动的回调任务） */
    key_sample_t keys[KEY_QUEUE_LEN];
    atomic_uint key_head;
    atomic_uint key_tail;
    /* 绘制耗时统计（只在渲染任务里读写） */
    uint32_t stats_t0;
    uint32_t stats_frames;
    uint64_t stats_total_us;
    uint32_t stats_max_us;
};

static void shell_draw(const esp_gsp_canvas_surface_t *surface, void *ctx)
{
    struct film_shell_t *s = ctx;
    for (uint16_t y = 0; y < surface->height; ++y) {
        const uint16_t *src = s->frame + (size_t)(surface->y + y) * FILM_APP_SCREEN_W + surface->x;
        memcpy((uint8_t *)surface->pixels + (size_t)y * surface->stride_bytes, src,
               (size_t)surface->width * sizeof(uint16_t));
    }
}

static void invalidate(esp_gsp_handle_t ui, struct film_shell_t *s)
{
    s->dirty = esp_gsp_canvas_invalidate(ui, s->config.canvas_bind) != ESP_GSP_OK;
}

static bool is_full(gfx_rect_t r)
{
    return r.x <= 0 && r.y <= 0 && r.w >= FILM_APP_SCREEN_W && r.h >= FILM_APP_SCREEN_H;
}

/** 只送 rect 这块到屏幕；送不出去就记下 dirty，下一帧整屏重画重送 */
static void invalidate_rect(esp_gsp_handle_t ui, struct film_shell_t *s, gfx_rect_t r)
{
    if (is_full(r)) {
        invalidate(ui, s);
        return;
    }
    const gsp_rect_t dirty = { .x1 = r.x, .y1 = r.y, .x2 = r.x + r.w, .y2 = r.y + r.h };
    s->dirty = esp_gsp_canvas_invalidate_dirty(ui, s->config.canvas_bind, dirty) != ESP_GSP_OK;
}

static bool stats_enabled(const struct film_shell_t *s)
{
    return s->config.now_us && s->config.report;
}

static void stats_add(struct film_shell_t *s, int64_t elapsed_us)
{
    const uint32_t us = elapsed_us > 0 ? (uint32_t)elapsed_us : 0;
    ++s->stats_frames;
    s->stats_total_us += us;
    if (us > s->stats_max_us) {
        s->stats_max_us = us;
    }
}

/** 周期到了就报告一次（周期内没有重绘就不报，静止画面不刷日志） */
static void stats_tick(struct film_shell_t *s, uint32_t now)
{
    if (!s->stats_t0) {
        s->stats_t0 = now ? now : 1;
        return;
    }
    const uint32_t period = now - s->stats_t0;
    if (period < STATS_PERIOD_MS) {
        return;
    }
    if (s->stats_frames) {
        const film_shell_stats_t stats = {
            .period_ms = period,
            .frames = s->stats_frames,
            .avg_us = (uint32_t)(s->stats_total_us / s->stats_frames),
            .max_us = s->stats_max_us,
        };
        s->config.report(&stats);
    }
    s->stats_t0 = now ? now : 1;
    s->stats_frames = 0;
    s->stats_total_us = 0;
    s->stats_max_us = 0;
}

/*
 * s->frame 跨帧保留整屏画面：只换了取景帧时界面只重画取景框（铝板、机身等静态部分沿用上一帧），
 * GSP 也只把这块送到屏幕。full 为 true 时（触摸、上次送屏失败）整屏重画。
 */
static void shell_render(esp_gsp_handle_t ui, struct film_shell_t *s, bool full)
{
    const gfx_rect_t rect = full ? gfx_rect(0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H) : film_app_damage(s->app);
    gfx_canvas_t canvas = {
        .pixels = s->frame,
        .width = FILM_APP_SCREEN_W,
        .height = FILM_APP_SCREEN_H,
        .stride = FILM_APP_SCREEN_W,
        .clip = rect,
    };
    const int64_t t0 = stats_enabled(s) ? s->config.now_us() : 0;
    film_app_render(s->app, &canvas);
    if (stats_enabled(s)) {
        stats_add(s, s->config.now_us() - t0);
    }
    invalidate_rect(ui, s, rect);
}

static esp_gsp_err_t create_app(struct film_shell_t *s, const film_port_t *port)
{
    const film_app_config_t app_config = { .port = port, .seed = s->config.seed };
    if (film_app_create(&app_config, &s->app) != ESP_OK) {
        return ESP_GSP_ERR_NO_MEM;
    }
    film_app_step(s->app, s->config.now_ms());
    return ESP_GSP_OK;
}

/* 开机动画阶段：播帧；播完后等平台就绪再换成界面 */
static void intro_tick(esp_gsp_handle_t ui, struct film_shell_t *s)
{
    const film_shell_intro_t *intro = s->config.intro;
    if (s->intro_playing) {
        s->intro_playing = intro->frame(intro->ctx, s->config.now_ms() - s->intro_t0, s->frame);
        invalidate(ui, s);
        return;
    }
    const film_port_t *port = intro->port(intro->ctx);
    if (port && create_app(s, port) == ESP_GSP_OK) {
        shell_render(ui, s, true);
    } else if (s->dirty) {
        invalidate(ui, s);
    }
}

static void apply_pointer(struct film_shell_t *s, int x, int y, bool pressed)
{
    if (s->app) {
        film_app_pointer(s->app, x, y, pressed, s->config.now_ms());
        s->dirty = true;
    }
}

/** 把远程触摸按到达顺序交给界面（渲染任务） */
static void drain_remote(struct film_shell_t *s)
{
    unsigned head = atomic_load_explicit(&s->remote_head, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&s->remote_tail, memory_order_acquire);
    while (head != tail) {
        const remote_sample_t sample = s->remote[head % REMOTE_QUEUE_LEN];
        /* 远程操作者看不到黑屏：直接唤醒，这一下照常生效 */
        film_app_wake(s->app, s->config.now_ms());
        apply_pointer(s, sample.x, sample.y, sample.pressed);
        ++head;
    }
    atomic_store_explicit(&s->remote_head, head, memory_order_release);
}

/** 把实体按键按到达顺序交给界面（渲染任务）；开机动画期间界面还没创建，按键直接丢弃 */
static void drain_keys(struct film_shell_t *s)
{
    unsigned head = atomic_load_explicit(&s->key_head, memory_order_relaxed);
    const unsigned tail = atomic_load_explicit(&s->key_tail, memory_order_acquire);
    while (head != tail) {
        const key_sample_t sample = s->keys[head % KEY_QUEUE_LEN];
        if (s->app) {
            film_app_key(s->app, (film_key_t)sample.key, sample.pressed, s->config.now_ms());
            s->dirty = true;
        }
        ++head;
    }
    atomic_store_explicit(&s->key_head, head, memory_order_release);
}

static void shell_tick(esp_gsp_handle_t ui, void *ctx)
{
    struct film_shell_t *s = ctx;
    drain_remote(s);
    drain_keys(s);
    if (!s->app) {
        intro_tick(ui, s);
    } else if (film_app_step(s->app, s->config.now_ms()) || s->dirty) {
        shell_render(ui, s, s->dirty);
    }
    if (s->app && stats_enabled(s)) {
        stats_tick(s, s->config.now_ms());
    }
}

static void shell_pointer(esp_gsp_handle_t ui, int32_t x, int32_t y, bool pressed, void *ctx)
{
    (void)ui;
    apply_pointer(ctx, (int)x, (int)y, pressed);
}

bool film_shell_post_pointer(film_shell_handle_t s, int x, int y, bool pressed)
{
    if (!s) {
        return false;
    }
    const unsigned tail = atomic_load_explicit(&s->remote_tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&s->remote_head, memory_order_acquire);
    if (tail - head >= REMOTE_QUEUE_LEN) {
        return false;
    }
    s->remote[tail % REMOTE_QUEUE_LEN] = (remote_sample_t){ .x = (int16_t)x, .y = (int16_t)y, .pressed = pressed };
    atomic_store_explicit(&s->remote_tail, tail + 1, memory_order_release);
    return true;
}

bool film_shell_post_key(film_shell_handle_t s, film_key_t key, bool pressed)
{
    if (!s) {
        return false;
    }
    const unsigned tail = atomic_load_explicit(&s->key_tail, memory_order_relaxed);
    const unsigned head = atomic_load_explicit(&s->key_head, memory_order_acquire);
    if (tail - head >= KEY_QUEUE_LEN) {
        return false;
    }
    s->keys[tail % KEY_QUEUE_LEN] = (key_sample_t){ .key = (uint8_t)key, .pressed = pressed };
    atomic_store_explicit(&s->key_tail, tail + 1, memory_order_release);
    return true;
}

esp_gsp_err_t film_shell_start(esp_gsp_handle_t ui, const film_shell_config_t *config,
                               film_shell_handle_t *ret_handle)
{
    const bool has_intro = config && config->intro && config->intro->frame && config->intro->port;
    if (!ui || !config || (!config->port && !has_intro) || !config->now_ms || !config->alloc || !config->free ||
        !ret_handle) {
        return ESP_GSP_ERR_INVALID_ARG;
    }
    struct film_shell_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_GSP_ERR_NO_MEM;
    }
    s->config = *config;
    s->frame = config->alloc(sizeof(uint16_t) * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
    esp_gsp_err_t err = s->frame ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
    if (err == ESP_GSP_OK && has_intro) {
        memset(s->frame, 0, sizeof(uint16_t) * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
        s->intro_t0 = config->now_ms();
        s->intro_playing = true;
    } else if (err == ESP_GSP_OK) {
        err = create_app(s, config->port);
        if (err == ESP_GSP_OK) {
            gfx_canvas_t canvas = {
                .pixels = s->frame, .width = FILM_APP_SCREEN_W, .height = FILM_APP_SCREEN_H,
                .stride = FILM_APP_SCREEN_W, .clip = { 0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H },
            };
            film_app_render(s->app, &canvas);
        }
    }
    if (err == ESP_GSP_OK) {
        err = esp_gsp_canvas_set_draw_cb(ui, config->canvas_bind, shell_draw, s);
    }
    if (err == ESP_GSP_OK) {
        err = esp_gsp_set_pointer_observer(ui, shell_pointer, s);
    }
    if (err == ESP_GSP_OK) {
        s->timer = esp_gsp_timer_create(ui, SHELL_TICK_MS, shell_tick, s);
        err = s->timer ? ESP_GSP_OK : ESP_GSP_ERR_NO_MEM;
    }
    if (err != ESP_GSP_OK) {
        film_shell_stop(ui, s);
        return err;
    }
    *ret_handle = s;
    return ESP_GSP_OK;
}

void film_shell_stop(esp_gsp_handle_t ui, film_shell_handle_t s)
{
    if (!s) {
        return;
    }
    if (s->timer) {
        esp_gsp_timer_delete(ui, s->timer);
    }
    esp_gsp_set_pointer_observer(ui, NULL, NULL);
    esp_gsp_canvas_stop(ui, s->config.canvas_bind);
    /* 等渲染任务里可能还在执行的回调结束后再释放 */
    esp_gsp_flush(ui, 1000);
    if (s->app) {
        film_app_delete(s->app);
    }
    if (s->frame) {
        s->config.free(s->frame);
    }
    free(s);
}

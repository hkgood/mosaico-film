#include "film_feedback.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "bsp/esp_mosaico.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "film_feedback";

#define SAMPLE_RATE     16000
#define CHUNK_SAMPLES   256                                 /*!< 每次混音 16 ms */
#define CHUNK_US        (CHUNK_SAMPLES * 1000000 / SAMPLE_RATE)
#define VOICES          3
#define REQUEST_QUEUE   8
#define TASK_STACK      4096
#define TASK_PRIORITY   6
#define SOUND_COUNT     (FILM_FEEDBACK_BOOT + 1)
#define TWO_PI_F        6.28318531f
#define PI_F            3.14159265f

typedef struct {
    int16_t *samples;
    uint32_t count;
} clip_t;

typedef struct {
    const clip_t *clip;
    uint32_t cursor;
} voice_t;

typedef struct {
    uint8_t strength;   /*!< 马达力度百分比，0 为停 */
    uint16_t ms;
} haptic_step_t;

typedef struct {
    const haptic_step_t *steps;
    uint8_t count;
} haptic_pattern_t;

/* 振动：快门是"咔—嚓"两下，拨盘是极短的一跳 */
static const haptic_step_t k_shutter[] = { { 85, 35 }, { 0, 45 }, { 60, 30 } };
static const haptic_step_t k_detent[] = { { 40, 12 } };
static const haptic_step_t k_click[] = { { 50, 20 } };
static const haptic_step_t k_lever[] = { { 40, 14 }, { 0, 30 }, { 40, 14 }, { 0, 30 }, { 70, 30 } };
static const haptic_step_t k_shake[] = { { 55, 140 } };
static const haptic_step_t k_error[] = { { 70, 60 }, { 0, 70 }, { 70, 60 } };
static const haptic_step_t k_boot[] = { { 95, 45 }, { 0, 50 }, { 70, 40 }, { 0, 120 }, { 35, 160 } };
#define PATTERN(s) { s, (uint8_t)(sizeof(s) / sizeof((s)[0])) }
static const haptic_pattern_t k_patterns[SOUND_COUNT] = {
    [FILM_FEEDBACK_SHUTTER] = PATTERN(k_shutter), [FILM_FEEDBACK_DETENT] = PATTERN(k_detent),
    [FILM_FEEDBACK_CLICK] = PATTERN(k_click),     [FILM_FEEDBACK_LEVER] = PATTERN(k_lever),
    [FILM_FEEDBACK_SHAKE] = PATTERN(k_shake),     [FILM_FEEDBACK_ERROR] = PATTERN(k_error),
    [FILM_FEEDBACK_BOOT] = PATTERN(k_boot),
};

/* 片段、混音声部与马达状态归混音任务独占；其他任务只往 requests 投递 */
struct film_feedback_t {
    esp_codec_dev_handle_t speaker;
    QueueHandle_t requests;
    bool motor_ready;
    clip_t clips[SOUND_COUNT];
    voice_t voices[VOICES];
    int16_t chunk[CHUNK_SAMPLES];
    const haptic_pattern_t *haptic;
    uint8_t haptic_step;
    int32_t haptic_left_us;
};

/* ---------------------------------------------------------------- 合成 */

static void mix(clip_t *c, uint32_t at, float v)
{
    if (at >= c->count) {
        return;
    }
    const float s = (float)c->samples[at] + v * 32767.0f;
    c->samples[at] = (int16_t)(s > 32767.0f ? 32767.0f : (s < -32768.0f ? -32768.0f : s));
}

/* 金属件被敲击：指数衰减的正弦 */
static void add_ring(clip_t *c, float start, float dur, float freq, float amp, float decay)
{
    const uint32_t first = (uint32_t)(start * SAMPLE_RATE), n = (uint32_t)(dur * SAMPLE_RATE);
    for (uint32_t i = 0; i < n; ++i) {
        const float t = (float)i / SAMPLE_RATE;
        mix(c, first + i, amp * fminf(1.0f, t * 2000.0f) * expf(-decay * t) * sinf(TWO_PI_F * freq * t));
    }
}

/* 机械撞击：快速起音、指数衰减的低通噪声；smooth 越小越闷 */
static void add_hit(clip_t *c, float start, float dur, float amp, float smooth, float decay, uint32_t *seed)
{
    const uint32_t first = (uint32_t)(start * SAMPLE_RATE), n = (uint32_t)(dur * SAMPLE_RATE);
    float lp = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        *seed = *seed * 1664525u + 1013904223u;
        const float white = (float)(*seed >> 8) / 8388608.0f - 1.0f;
        lp += (white - lp) * smooth;
        mix(c, first + i, amp * expf(-decay * (float)i / SAMPLE_RATE) * lp);
    }
}

/* 音高滑动 + sin 包络（咚、嗡） */
static void add_glide(clip_t *c, float start, float dur, float f0, float f1, float amp)
{
    const uint32_t first = (uint32_t)(start * SAMPLE_RATE), n = (uint32_t)(dur * SAMPLE_RATE);
    float phase = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const float t = (float)i / (float)n;
        phase += TWO_PI_F * (f0 * powf(f1 / f0, t)) / SAMPLE_RATE;
        mix(c, first + i, amp * sinf(PI_F * t) * sinf(phase));
    }
}

/* 风声：带包络的暗噪声 */
static void add_whoosh(clip_t *c, float start, float dur, float amp, uint32_t *seed)
{
    const uint32_t first = (uint32_t)(start * SAMPLE_RATE), n = (uint32_t)(dur * SAMPLE_RATE);
    float lp = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        *seed = *seed * 1664525u + 1013904223u;
        const float t = (float)i / (float)n;
        const float smooth = 0.03f + 0.12f * sinf(PI_F * t);
        lp += ((float)(*seed >> 8) / 8388608.0f - 1.0f - lp) * smooth;
        mix(c, first + i, amp * sinf(PI_F * t) * lp);
    }
}

/* 一次快门："咔"（快门帘起跑 + 反光镜）—"嚓"（后帘闭合） */
static void add_shutter(clip_t *c, float start, float weight, uint32_t *seed)
{
    add_hit(c, start, 0.03f, 0.55f * weight, 0.75f, 160.0f, seed);
    add_glide(c, start, 0.035f, 180.0f, 90.0f, 0.35f * weight);
    add_ring(c, start + 0.002f, 0.05f, 2350.0f, 0.10f * weight, 90.0f);
    add_hit(c, start + 0.065f, 0.03f, 0.45f * weight, 0.6f, 180.0f, seed);
    add_ring(c, start + 0.066f, 0.06f, 1650.0f, 0.10f * weight, 70.0f);
}

/* 按 int 分支：FILM_FEEDBACK_BOOT 是本模块在界面枚举之外追加的值 */
static float clip_seconds(film_feedback_t kind)
{
    switch ((int)kind) {
    case FILM_FEEDBACK_SHUTTER: return 0.16f;
    case FILM_FEEDBACK_DETENT: return 0.025f;
    case FILM_FEEDBACK_CLICK: return 0.05f;
    case FILM_FEEDBACK_LEVER: return 0.20f;
    case FILM_FEEDBACK_SHAKE: return 0.30f;
    case FILM_FEEDBACK_ERROR: return 0.30f;
    case FILM_FEEDBACK_BOOT: return 0.70f;
    default: return 0.0f;
    }
}

static void synthesize(film_feedback_t kind, clip_t *c)
{
    uint32_t seed = 0xF11Au + (uint32_t)kind;
    switch ((int)kind) {
    case FILM_FEEDBACK_SHUTTER:
        add_shutter(c, 0.0f, 1.0f, &seed);
        break;
    case FILM_FEEDBACK_DETENT:
        add_hit(c, 0.0f, 0.012f, 0.30f, 0.9f, 400.0f, &seed);
        add_ring(c, 0.0f, 0.02f, 3200.0f, 0.06f, 220.0f);
        break;
    case FILM_FEEDBACK_CLICK:
        add_hit(c, 0.0f, 0.02f, 0.35f, 0.7f, 260.0f, &seed);
        add_glide(c, 0.0f, 0.03f, 1300.0f, 900.0f, 0.15f);
        break;
    case FILM_FEEDBACK_LEVER:
        /* 过片扳手：棘轮两响，最后"咔哒"到位 */
        add_hit(c, 0.0f, 0.015f, 0.22f, 0.85f, 300.0f, &seed);
        add_hit(c, 0.045f, 0.015f, 0.24f, 0.85f, 300.0f, &seed);
        add_hit(c, 0.09f, 0.04f, 0.45f, 0.55f, 120.0f, &seed);
        add_ring(c, 0.091f, 0.08f, 1900.0f, 0.08f, 60.0f);
        break;
    case FILM_FEEDBACK_SHAKE:
        add_whoosh(c, 0.0f, 0.30f, 0.45f, &seed);
        break;
    case FILM_FEEDBACK_ERROR:
        add_glide(c, 0.0f, 0.12f, 240.0f, 200.0f, 0.30f);
        add_glide(c, 0.15f, 0.14f, 200.0f, 160.0f, 0.30f);
        break;
    case FILM_FEEDBACK_BOOT:
        add_shutter(c, 0.0f, 1.25f, &seed);
        add_glide(c, 0.0f, 0.12f, 110.0f, 55.0f, 0.35f);
        /* 胶片推进的马达嗡声 */
        add_glide(c, 0.24f, 0.40f, 300.0f, 360.0f, 0.10f);
        add_whoosh(c, 0.24f, 0.40f, 0.15f, &seed);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- 混音任务 */

static void set_motor(film_feedback_handle_t h, uint8_t strength)
{
    if (h->motor_ready && bsp_motor_set_strength(strength) != ESP_OK) {
        h->motor_ready = false;
    }
}

static void start(film_feedback_handle_t h, film_feedback_t kind)
{
    if ((int)kind < 0 || kind >= SOUND_COUNT) {
        return;
    }
    if (h->clips[kind].samples) {
        voice_t *slot = &h->voices[0];
        for (int i = 0; i < VOICES; ++i) {
            if (!h->voices[i].clip) {
                slot = &h->voices[i];
                break;
            }
            /* 声部满了：抢最快结束的那个 */
            if (h->voices[i].clip->count - h->voices[i].cursor < slot->clip->count - slot->cursor) {
                slot = &h->voices[i];
            }
        }
        *slot = (voice_t){ &h->clips[kind], 0 };
    }
    if (h->motor_ready) {
        h->haptic = &k_patterns[kind];
        h->haptic_step = 0;
        h->haptic_left_us = (int32_t)h->haptic->steps[0].ms * 1000;
        set_motor(h, h->haptic->steps[0].strength);
    }
}

static void advance_haptic(film_feedback_handle_t h, int32_t elapsed_us)
{
    if (!h->haptic) {
        return;
    }
    h->haptic_left_us -= elapsed_us;
    while (h->haptic && h->haptic_left_us <= 0) {
        if (++h->haptic_step >= h->haptic->count) {
            h->haptic = NULL;
            set_motor(h, 0);
            return;
        }
        const haptic_step_t *step = &h->haptic->steps[h->haptic_step];
        h->haptic_left_us += (int32_t)step->ms * 1000;
        set_motor(h, step->strength);
    }
}

static bool voices_active(film_feedback_handle_t h)
{
    for (int i = 0; i < VOICES; ++i) {
        if (h->voices[i].clip) {
            return true;
        }
    }
    return false;
}

static void mix_chunk(film_feedback_handle_t h)
{
    for (int s = 0; s < CHUNK_SAMPLES; ++s) {
        int32_t sum = 0;
        for (int i = 0; i < VOICES; ++i) {
            voice_t *v = &h->voices[i];
            if (v->clip) {
                sum += v->clip->samples[v->cursor];
                if (++v->cursor >= v->clip->count) {
                    v->clip = NULL;
                }
            }
        }
        h->chunk[s] = (int16_t)(sum > 32767 ? 32767 : (sum < -32768 ? -32768 : sum));
    }
}

static void mixer_task(void *arg)
{
    film_feedback_handle_t h = arg;
    for (;;) {
        film_feedback_t kind;
        /* 没有声音和振动时一直睡到下一个请求 */
        const TickType_t wait = (voices_active(h) || h->haptic) ? 0 : portMAX_DELAY;
        while (xQueueReceive(h->requests, &kind, wait) == pdTRUE) {
            start(h, kind);
            if (wait == portMAX_DELAY) {
                break;
            }
        }
        if (h->speaker && voices_active(h)) {
            mix_chunk(h);
            if (esp_codec_dev_write(h->speaker, h->chunk, sizeof(h->chunk)) != ESP_CODEC_DEV_OK) {
                vTaskDelay(pdMS_TO_TICKS(CHUNK_US / 1000));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(CHUNK_US / 1000));
        }
        advance_haptic(h, CHUNK_US);
    }
}

/* ---------------------------------------------------------------- 创建 */

static esp_err_t open_speaker(film_feedback_handle_t h, uint8_t volume)
{
    i2s_std_config_t i2s = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_AUDIO_I2S_MCLK,
            .bclk = BSP_AUDIO_I2S_SCLK,
            .ws = BSP_AUDIO_I2S_LRCLK,
            .dout = BSP_AUDIO_I2S_SDOUT,
            .din = BSP_AUDIO_I2S_DSIN,
        },
    };
    i2s.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(bsp_audio_init(&i2s), TAG, "I2S init");
    h->speaker = bsp_audio_codec_speaker_init();
    ESP_RETURN_ON_FALSE(h->speaker, ESP_FAIL, TAG, "speaker codec unavailable");
    esp_codec_dev_sample_info_t info = { .sample_rate = SAMPLE_RATE, .bits_per_sample = 16, .channel = 1 };
    if (esp_codec_dev_open(h->speaker, &info) != ESP_CODEC_DEV_OK) {
        h->speaker = NULL;
        return ESP_FAIL;
    }
    (void)esp_codec_dev_set_out_vol(h->speaker, volume);
    return ESP_OK;
}

esp_err_t film_feedback_create(const film_feedback_config_t *config, film_feedback_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(config && ret_handle, ESP_ERR_INVALID_ARG, TAG, "bad args");
    film_feedback_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "feedback");
    for (int kind = 0; kind < SOUND_COUNT; ++kind) {
        clip_t *c = &h->clips[kind];
        c->count = (uint32_t)(clip_seconds((film_feedback_t)kind) * SAMPLE_RATE);
        c->samples = heap_caps_calloc(c->count, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (c->samples) {
            synthesize((film_feedback_t)kind, c);
        }
    }
    const esp_err_t speaker = open_speaker(h, config->volume_percent > 100 ? 100 : config->volume_percent);
    if (speaker != ESP_OK) {
        ESP_LOGW(TAG, "speaker unavailable: %s", esp_err_to_name(speaker));
    }
    h->motor_ready = bsp_motor_init() == ESP_OK;
    if (!h->motor_ready) {
        ESP_LOGW(TAG, "motor unavailable");
    }
    h->requests = xQueueCreate(REQUEST_QUEUE, sizeof(film_feedback_t));
    if (!h->requests || xTaskCreatePinnedToCore(mixer_task, "film_fb", TASK_STACK, h, TASK_PRIORITY, NULL,
                                                config->core) != pdPASS) {
        if (h->requests) {
            vQueueDelete(h->requests);
        }
        for (int i = 0; i < SOUND_COUNT; ++i) {
            heap_caps_free(h->clips[i].samples);
        }
        free(h);
        return ESP_ERR_NO_MEM;
    }
    *ret_handle = h;
    return ESP_OK;
}

void film_feedback_play(film_feedback_handle_t handle, film_feedback_t kind)
{
    if (handle) {
        (void)xQueueSend(handle->requests, &kind, 0);
    }
}

#include "film_boot.h"

#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "film_boot";

#define BOOT_MAGIC          "MFB1"
#define BOOT_HEADER_BYTES   8           /*!< magic[4] + count u16 + fps u16 */
#define BOOT_SIDE           480
#define BOOT_PIXEL_BYTES    (BOOT_SIDE * BOOT_SIDE * 2)
#define BOOT_IN_CAPACITY    (64 * 1024) /*!< 单帧 JPEG 实测最大约 11 KB */
#define BOOT_SOUND_MS       1600        /*!< 动画里快门落下的时刻 */
#define DECODE_TIMEOUT_MS   200

struct film_boot_t {
    const uint8_t *data;
    size_t size;
    uint16_t count;
    uint16_t fps;
    film_feedback_handle_t feedback;
    jpeg_decoder_handle_t decoder;
    uint8_t *in;                /*!< 帧 JPEG 的 DMA 副本（flash 映射区不能直接给 DMA） */
    size_t in_capacity;
    uint8_t *out;               /*!< 解码输出 RGB565 */
    size_t out_capacity;
    int shown;                  /*!< 已显示的帧号，-1 表示还没开始 */
    bool sound_played;
};

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool frame_span(film_boot_handle_t b, int index, const uint8_t **ret, size_t *ret_size)
{
    const uint8_t *table = b->data + BOOT_HEADER_BYTES;
    const uint32_t start = read_u32(table + 4u * (uint32_t)index);
    const uint32_t end = read_u32(table + 4u * (uint32_t)(index + 1));
    if (start >= end || end > b->size || end - start > b->in_capacity) {
        return false;
    }
    *ret = b->data + start;
    *ret_size = end - start;
    return true;
}

static esp_err_t decode(film_boot_handle_t b, int index, uint16_t *pixels)
{
    const uint8_t *jpeg;
    size_t size;
    ESP_RETURN_ON_FALSE(frame_span(b, index, &jpeg, &size), ESP_ERR_INVALID_SIZE, TAG, "frame %d", index);
    memcpy(b->in, jpeg, size);
    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,     /* 小端 RGB565，与界面画面相同 */
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    ESP_RETURN_ON_ERROR(jpeg_decoder_process(b->decoder, &config, b->in, (uint32_t)size, b->out,
                                             (uint32_t)b->out_capacity, &out_size), TAG, "decode %d", index);
    ESP_RETURN_ON_FALSE(out_size >= BOOT_PIXEL_BYTES, ESP_ERR_INVALID_SIZE, TAG, "frame size %u", (unsigned)out_size);
    memcpy(pixels, b->out, BOOT_PIXEL_BYTES);
    return ESP_OK;
}

bool film_boot_frame(void *ctx, uint32_t t_ms, uint16_t *pixels)
{
    film_boot_handle_t b = ctx;
    if (!b->sound_played && t_ms >= BOOT_SOUND_MS) {
        b->sound_played = true;
        film_feedback_play(b->feedback, FILM_FEEDBACK_BOOT);
    }
    int index = (int)((uint64_t)t_ms * b->fps / 1000u);
    const bool done = index >= b->count - 1;
    if (done) {
        index = b->count - 1;
    }
    /* 渲染慢于 30 fps 时直接跳到当前该显示的帧 */
    if (index != b->shown) {
        if (decode(b, index, pixels) == ESP_OK) {
            b->shown = index;
        } else if (b->shown < 0) {
            return false;   /* 第一帧都解不出：直接进界面 */
        }
    }
    return !done;
}

void film_boot_delete(film_boot_handle_t b)
{
    if (!b) {
        return;
    }
    if (b->decoder) {
        (void)jpeg_del_decoder_engine(b->decoder);
    }
    heap_caps_free(b->in);
    heap_caps_free(b->out);
    free(b);
}

esp_err_t film_boot_create(const uint8_t *data, size_t size, film_feedback_handle_t feedback,
                           film_boot_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(data && size > BOOT_HEADER_BYTES && ret_handle, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(memcmp(data, BOOT_MAGIC, 4) == 0, ESP_ERR_INVALID_VERSION, TAG, "not MFB1");
    film_boot_handle_t b = calloc(1, sizeof(*b));
    ESP_RETURN_ON_FALSE(b, ESP_ERR_NO_MEM, TAG, "boot");
    b->data = data;
    b->size = size;
    b->count = (uint16_t)(data[4] | (data[5] << 8));
    b->fps = (uint16_t)(data[6] | (data[7] << 8));
    b->feedback = feedback;
    b->shown = -1;
    esp_err_t err = (b->count > 0 && b->fps > 0 &&
                     BOOT_HEADER_BYTES + 4u * (b->count + 1u) <= size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        const jpeg_decode_engine_cfg_t engine = { .timeout_ms = DECODE_TIMEOUT_MS };
        err = jpeg_new_decoder_engine(&engine, &b->decoder);
    }
    if (err == ESP_OK) {
        const jpeg_decode_memory_alloc_cfg_t in_cfg = { .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER };
        const jpeg_decode_memory_alloc_cfg_t out_cfg = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        b->in = jpeg_alloc_decoder_mem(BOOT_IN_CAPACITY, &in_cfg, &b->in_capacity);
        b->out = jpeg_alloc_decoder_mem(BOOT_PIXEL_BYTES, &out_cfg, &b->out_capacity);
        err = b->in && b->out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create: %s", esp_err_to_name(err));
        film_boot_delete(b);
        return err;
    }
    ESP_LOGI(TAG, "boot animation: %u frames @ %u fps", b->count, b->fps);
    *ret_handle = b;
    return ESP_OK;
}

#include "film_discharge.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "film_power";

#define NVS_NAMESPACE   "film_pwr"
#define NVS_KEY         "discharge"
#define RECORD_VERSION  1
#define SAVE_EVERY      4       /*!< 每几次读数写一次 NVS（15 s 一次读数时约 1 分钟） */
#define STATES          (FILM_DISPLAY_OFF + 1)
#define CORES           2
#define FW_TAG_LEN      9       /*!< ELF SHA-256 前 8 个十六进制字符 + 结尾 */

/** 一种屏幕状态下的累计 */
typedef struct {
    uint32_t samples;
    int32_t sum_ma;
    uint32_t cpu_samples;
    uint32_t cpu_sum[CORES];
} bucket_t;

/** 存进 NVS 的整段记录（版本号变了就当作没有记录） */
typedef struct {
    uint16_t version;
    uint16_t boots;             /*!< 这段记录跨了几次开机（中途断电或重启会加一） */
    uint32_t period_ms;
    uint8_t start_soc;
    uint8_t last_soc;
    int16_t start_mv;
    int16_t last_mv;
    bucket_t bucket[STATES];
    char fw[FW_TAG_LEN];
} record_t;

struct film_discharge_t {
    record_t rec;
    bool active;                /*!< rec 里有一段还没结束的记录 */
    uint32_t unsaved;           /*!< 上次写 NVS 之后又记了几次 */
    uint32_t period_ms;
    char fw[FW_TAG_LEN];        /*!< 当前固件 */
};

static void save(film_discharge_handle_t d)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_blob(nvs, NVS_KEY, &d->rec, sizeof(d->rec));
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "discharge record save: %s", esp_err_to_name(err));
    }
    d->unsaved = 0;
}

static void erase(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        (void)nvs_erase_key(nvs, NVS_KEY);
        (void)nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static bool load(record_t *ret_rec)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t size = sizeof(*ret_rec);
    const esp_err_t err = nvs_get_blob(nvs, NVS_KEY, ret_rec, &size);
    nvs_close(nvs);
    return err == ESP_OK && size == sizeof(*ret_rec) && ret_rec->version == RECORD_VERSION;
}

/** 一种屏幕状态的摘要："on 120x -142 mA cpu 47/43%"，没有读数时为空 */
static void format_bucket(const char *name, const bucket_t *b, char *out, size_t size)
{
    out[0] = '\0';
    if (!b->samples) {
        return;
    }
    const int mean = (int)(b->sum_ma / (int32_t)b->samples);
    if (b->cpu_samples) {
        snprintf(out, size, " | %s %" PRIu32 "x %d mA cpu %" PRIu32 "/%" PRIu32 "%%", name, b->samples, mean,
                 b->cpu_sum[0] / b->cpu_samples, b->cpu_sum[1] / b->cpu_samples);
    } else {
        snprintf(out, size, " | %s %" PRIu32 "x %d mA", name, b->samples, mean);
    }
}

static void print(const record_t *r)
{
    static const char *const k_names[STATES] = { "on", "dim", "off" };
    uint32_t samples = 0;
    int32_t sum = 0;
    char parts[STATES][64];
    for (int i = 0; i < STATES; ++i) {
        samples += r->bucket[i].samples;
        sum += r->bucket[i].sum_ma;
        format_bucket(k_names[i], &r->bucket[i], parts[i], sizeof(parts[i]));
    }
    if (!samples) {
        return;
    }
    ESP_LOGI(TAG,
             "discharge: fw %.8s, %u boot(s), %" PRIu32 " s, SoC %u%% -> %u%%, %d -> %d mV, mean %d mA%s%s%s",
             r->fw, (unsigned)r->boots, samples * (r->period_ms / 1000u), (unsigned)r->start_soc,
             (unsigned)r->last_soc, r->start_mv, r->last_mv, (int)(sum / (int32_t)samples), parts[0], parts[1],
             parts[2]);
}

esp_err_t film_discharge_create(uint32_t period_ms, film_discharge_handle_t *ret_handle)
{
    ESP_RETURN_ON_FALSE(ret_handle && period_ms, ESP_ERR_INVALID_ARG, TAG, "bad args");
    film_discharge_handle_t d = calloc(1, sizeof(*d));
    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "discharge");
    d->period_ms = period_ms;
    char sha[FW_TAG_LEN];
    (void)esp_app_get_elf_sha256(sha, sizeof(sha));
    memcpy(d->fw, sha, sizeof(d->fw));

    if (load(&d->rec)) {
        const bool same_fw = memcmp(d->rec.fw, d->fw, FW_TAG_LEN) == 0 && d->rec.period_ms == period_ms;
        if (same_fw) {
            ++d->rec.boots;
            d->active = true;
            ESP_LOGI(TAG, "discharge record resumed (boot %u)", (unsigned)d->rec.boots);
        } else {
            print(&d->rec);   /* 另一版固件留下的：先交出来，再从头记 */
            erase();
        }
    }
    *ret_handle = d;
    return ESP_OK;
}

void film_discharge_add(film_discharge_handle_t d, const film_discharge_sample_t *s)
{
    if (!d || !s) {
        return;
    }
    record_t *r = &d->rec;
    if (!d->active) {
        *r = (record_t){
            .version = RECORD_VERSION, .boots = 1, .period_ms = d->period_ms,
            .start_soc = s->soc, .start_mv = s->voltage_mv,
        };
        memcpy(r->fw, d->fw, sizeof(r->fw));
        d->active = true;
    }
    const unsigned state = (unsigned)s->display < STATES ? (unsigned)s->display : FILM_DISPLAY_ON;
    bucket_t *b = &r->bucket[state];
    ++b->samples;
    b->sum_ma += s->average_ma;
    if (s->cpu_valid) {
        ++b->cpu_samples;
        b->cpu_sum[0] += s->cpu[0];
        b->cpu_sum[1] += s->cpu[1];
    }
    r->last_soc = s->soc;
    r->last_mv = s->voltage_mv;
    if (++d->unsaved >= SAVE_EVERY) {
        save(d);
    }
}

void film_discharge_end(film_discharge_handle_t d)
{
    if (!d || !d->active) {
        return;
    }
    print(&d->rec);
    erase();
    d->active = false;
    d->unsaved = 0;
}

void film_discharge_delete(film_discharge_handle_t d)
{
    if (!d) {
        return;
    }
    if (d->active && d->unsaved) {
        save(d);
    }
    free(d);
}

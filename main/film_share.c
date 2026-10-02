#include "film_share.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "film_library.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "film_share";

/* Vibe Mode 保存的 Wi-Fi（只读，绝不改写） */
#define VIBE_PARTITION      "sysmeta"
#define VIBE_WIFI_NAMESPACE "wifi"
#define SSID_KEY            "ssid"
#define PASSWORD_KEY        "password"
#define SSID_SIZE           33
#define PASSWORD_SIZE       65          /*!< 可能是 64 位十六进制密钥 */
#define SNTP_SERVER         "ntp.aliyun.com"

#define TASK_STACK          4096
#define COMMAND_QUEUE_LEN   4
#define TICK_MS             200
#define STA_CONNECT_MS      15000       /*!< 分享时连路由器的最长等待，超时改开热点 */
#define TIME_SYNC_MS        45000       /*!< 开机校时最长保持 Wi-Fi 的时间 */
#define AP_CHANNEL          6
#define AP_MAX_CLIENTS      2
#define AP_PASSWORD_LEN     8
#define AP_IP               "192.168.4.1"
#define TOKEN_LEN           6
#define MAX_SHARE           64          /*!< 一次最多分享的张数 */
#define FILE_CHUNK          4096
#define HTTP_STACK          6144

/* Wi-Fi 事件 → 分享任务的通知位 */
#define BIT_GOT_IP          (1u << 0)
#define BIT_STA_LOST        (1u << 1)
#define BIT_AP_JOINED       (1u << 2)
#define BIT_COMMAND         (1u << 3)

typedef enum {
    CMD_BEGIN = 0,
    CMD_END,
} command_kind_t;

typedef struct {
    command_kind_t kind;
    bool hotspot;
} command_t;

typedef enum {
    RADIO_OFF = 0,
    RADIO_TIME_SYNC,        /*!< 开机连路由器校时 */
    RADIO_STA_CONNECTING,   /*!< 分享：正在连路由器 */
    RADIO_LAN,
    RADIO_AP,
} radio_t;

/* 单例。lock 保护 status / ids / root / token；其余只在分享任务里使用 */
static struct {
    bool initialized;
    film_share_config_t config;
    TaskHandle_t task;
    QueueHandle_t commands;
    SemaphoreHandle_t lock;
    esp_netif_t *sta;
    esp_netif_t *ap;
    httpd_handle_t http;
    radio_t radio;
    bool wifi_inited;       /*!< 驱动只在校时/分享期间存在，用完即释放内存 */
    bool wifi_started;
    int64_t deadline_us;
    char ssid[SSID_SIZE];
    char password[PASSWORD_SIZE];
    bool have_credentials;

    film_share_status_t status;
    const char *root;
    char token[TOKEN_LEN + 1];
    uint32_t ids[MAX_SHARE];
    size_t count;
    uint64_t downloaded;    /*!< 每张照片是否被保存过（按 ids 下标） */
} s_share;

static atomic_bool s_time_synced;

/* ---------------------------------------------------------------- 状态 */

static void publish(void)
{
    const film_event_t ev = { .type = FILM_EVT_SHARE };
    (void)xQueueSend(s_share.config.events, &ev, 0);
}

static void set_phase(film_share_phase_t phase)
{
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    s_share.status.phase = phase;
    xSemaphoreGive(s_share.lock);
    publish();
}

void film_share_get_status(film_share_status_t *ret_status)
{
    if (!s_share.initialized) {
        memset(ret_status, 0, sizeof(*ret_status));
        return;
    }
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    *ret_status = s_share.status;
    xSemaphoreGive(s_share.lock);
}

bool film_share_time_synced(void)
{
    return atomic_load(&s_time_synced);
}

void film_share_set_root(const char *root)
{
    if (!s_share.initialized) {
        return;
    }
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    s_share.root = root;
    xSemaphoreGive(s_share.lock);
}

/* ---------------------------------------------------------------- 凭据 */

static bool load_credentials(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open_from_partition(VIBE_PARTITION, VIBE_WIFI_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_PART_NOT_FOUND || err == ESP_ERR_NVS_NOT_INITIALIZED) {
        /* 只初始化（不擦除）已存在的分区 */
        if (nvs_flash_init_partition(VIBE_PARTITION) == ESP_OK) {
            err = nvs_open_from_partition(VIBE_PARTITION, VIBE_WIFI_NAMESPACE, NVS_READONLY, &nvs);
        }
    }
    if (err != ESP_OK) {
        return false;
    }
    size_t ssid_size = sizeof(s_share.ssid), password_size = sizeof(s_share.password);
    err = nvs_get_str(nvs, SSID_KEY, s_share.ssid, &ssid_size);
    if (err == ESP_OK && nvs_get_str(nvs, PASSWORD_KEY, s_share.password, &password_size) != ESP_OK) {
        s_share.password[0] = '\0';   /* 开放网络 */
    }
    nvs_close(nvs);
    return err == ESP_OK && s_share.ssid[0];
}

/* ---------------------------------------------------------------- 网页 */

static bool token_ok(const char *token, size_t len)
{
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    const bool ok = s_share.token[0] && len == TOKEN_LEN && memcmp(token, s_share.token, TOKEN_LEN) == 0;
    xSemaphoreGive(s_share.lock);
    return ok;
}

static esp_err_t send_gallery(httpd_req_t *req)
{
    /* 页面行缓冲后面接一份编号快照（都放堆上，httpd 任务栈很小） */
    char *line = malloc(FILE_CHUNK + sizeof(uint32_t) * MAX_SHARE);
    ESP_RETURN_ON_FALSE(line, ESP_ERR_NO_MEM, TAG, "page buffer");
    uint32_t *ids = (uint32_t *)(line + FILE_CHUNK);
    char token[TOKEN_LEN + 1];
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    const size_t count = s_share.count;
    memcpy(ids, s_share.ids, sizeof(ids[0]) * count);
    memcpy(token, s_share.token, sizeof(token));
    s_share.status.visits++;
    xSemaphoreGive(s_share.lock);
    publish();

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html lang=zh><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'><title>Mosaico Film</title><style>"
        "body{margin:0;background:#0e0e0d;color:#efe7d6;font:15px -apple-system,Helvetica,sans-serif}"
        "header{padding:26px 18px 6px}h1{font:600 21px Georgia,serif;letter-spacing:.12em;margin:0}"
        "header p{color:#9c958a;margin:8px 0 0;line-height:1.5}.g{display:grid;gap:16px;padding:14px;"
        "grid-template-columns:repeat(auto-fill,minmax(260px,1fr))}figure{margin:0;background:#1b1a18;"
        "border-radius:8px;overflow:hidden;box-shadow:0 6px 20px #0008}img{width:100%;display:block;"
        "background:#26241f;min-height:120px}a{display:block;text-align:center;padding:12px;color:#ff9a3c;"
        "text-decoration:none;font-weight:600;letter-spacing:.06em}footer{color:#5d5850;text-align:center;"
        "padding:18px;font-size:12px}</style><header><h1>MOSAICO FILM</h1>");
    snprintf(line, FILE_CHUNK, "<p>%u 张照片 · 点「保存」或长按图片存到手机</p></header><div class=g>",
             (unsigned)count);
    httpd_resp_sendstr_chunk(req, line);
    for (size_t i = 0; i < count; ++i) {
        snprintf(line, FILE_CHUNK,
                 "<figure><img loading=lazy src='/s/%s/%lu.jpg'><a href='/s/%s/%lu.jpg?dl=1' "
                 "download='MOSAICO_%lu.jpg'>保存</a></figure>",
                 token, (unsigned long)ids[i], token, (unsigned long)ids[i], (unsigned long)ids[i]);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, "</div><footer>ESP-MOSAICO · FILM</footer></html>");
    free(line);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t send_photo(httpd_req_t *req, uint32_t id, bool download)
{
    char path[FILM_PATH_MAX];
    int index = -1;
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    for (size_t i = 0; i < s_share.count; ++i) {
        if (s_share.ids[i] == id) {
            index = (int)i;
        }
    }
    const char *root = s_share.root;
    if (index >= 0 && root) {
        film_library_path(root, id, FILM_FILE_JPEG, path, sizeof(path));
    }
    xSemaphoreGive(s_share.lock);
    if (index < 0 || !root) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not shared");
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "missing");
    }
    char *chunk = malloc(FILE_CHUNK);
    if (!chunk) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=600");
    char disposition[48];
    if (download) {
        snprintf(disposition, sizeof(disposition), "attachment; filename=\"MOSAICO_%lu.jpg\"", (unsigned long)id);
        httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    }
    esp_err_t err = ESP_OK;
    size_t n;
    while (err == ESP_OK && (n = fread(chunk, 1, FILE_CHUNK, f)) > 0) {
        err = httpd_resp_send_chunk(req, chunk, (ssize_t)n);
    }
    fclose(f);
    free(chunk);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    if (err == ESP_OK && download) {
        xSemaphoreTake(s_share.lock, portMAX_DELAY);
        if (index < 64 && !(s_share.downloaded & (1ull << index))) {
            s_share.downloaded |= 1ull << index;
            s_share.status.downloads++;
        }
        xSemaphoreGive(s_share.lock);
        publish();
    }
    return err;
}

/* /s/<令牌> 或 /s/<令牌>/ 是相册页，/s/<令牌>/<编号>.jpg 是照片 */
static esp_err_t on_request(httpd_req_t *req)
{
    const char *uri = req->uri + 3;   /* 跳过 "/s/" */
    const char *slash = strchr(uri, '/');
    const char *query = strchr(uri, '?');
    const char *token_end = slash ? slash : (query ? query : uri + strlen(uri));
    if (!token_ok(uri, (size_t)(token_end - uri))) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "session expired");
    }
    if (!slash || slash[1] == '\0' || slash[1] == '?') {
        return send_gallery(req);
    }
    char *end = NULL;
    const unsigned long id = strtoul(slash + 1, &end, 10);
    if (!end || strncmp(end, ".jpg", 4) != 0 || id == 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "bad photo");
    }
    const bool download = query && strstr(query, "dl=1");
    return send_photo(req, (uint32_t)id, download);
}

static esp_err_t http_start(void)
{
    if (s_share.http) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = HTTP_STACK;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.core_id = s_share.config.core;
    ESP_RETURN_ON_ERROR(httpd_start(&s_share.http, &config), TAG, "httpd start");
    const httpd_uri_t gallery = { .uri = "/s/*", .method = HTTP_GET, .handler = on_request };
    const esp_err_t err = httpd_register_uri_handler(s_share.http, &gallery);
    if (err != ESP_OK) {
        httpd_stop(s_share.http);
        s_share.http = NULL;
    }
    return err;
}

static void http_stop(void)
{
    if (s_share.http) {
        httpd_stop(s_share.http);
        s_share.http = NULL;
    }
}

/* ---------------------------------------------------------------- Wi-Fi */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    uint32_t bits = 0;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bits = BIT_STA_LOST;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        bits = BIT_AP_JOINED;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        bits = BIT_GOT_IP;
    }
    if (bits && s_share.task) {
        xTaskNotify(s_share.task, bits, eSetBits);
    }
}

static void on_time_sync(struct timeval *tv)
{
    (void)tv;
    atomic_store(&s_time_synced, true);
    ESP_LOGI(TAG, "clock synchronized");
}

/* Wi-Fi 驱动常驻会占用约 50 KB 内部 RAM，而相机 DMA 也要内部 RAM，所以按需创建 */
static esp_err_t wifi_driver_up(void)
{
    if (s_share.wifi_inited) {
        return ESP_OK;
    }
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi init");
    /* 凭据由 Vibe Mode 管理：驱动不得把任何配置写回 NVS */
    const esp_err_t err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        (void)esp_wifi_deinit();
        ESP_LOGE(TAG, "wifi storage: %s", esp_err_to_name(err));
        return err;
    }
    s_share.wifi_inited = true;
    return ESP_OK;
}

static void radio_off(void)
{
    http_stop();
    if (s_share.wifi_started) {
        (void)esp_wifi_stop();
        s_share.wifi_started = false;
    }
    if (s_share.wifi_inited) {
        (void)esp_wifi_deinit();
        s_share.wifi_inited = false;
    }
    s_share.radio = RADIO_OFF;
}

static esp_err_t sta_connect(void)
{
    radio_off();
    ESP_RETURN_ON_ERROR(wifi_driver_up(), TAG, "wifi driver");
    wifi_config_t config = { 0 };
    strlcpy((char *)config.sta.ssid, s_share.ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, s_share.password, sizeof(config.sta.password));
    config.sta.threshold.authmode = s_share.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    s_share.wifi_started = true;
    return esp_wifi_connect();
}

static void random_text(char *buf, size_t len, const char *alphabet)
{
    const size_t n = strlen(alphabet);
    for (size_t i = 0; i < len; ++i) {
        buf[i] = alphabet[esp_random() % n];
    }
    buf[len] = '\0';
}

static esp_err_t ap_start(void)
{
    radio_off();
    ESP_RETURN_ON_ERROR(wifi_driver_up(), TAG, "wifi driver");
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[24], password[AP_PASSWORD_LEN + 1];
    snprintf(ssid, sizeof(ssid), "Mosaico-Film-%02X%02X", mac[4], mac[5]);
    /* 去掉易混的 0/o/1/l，方便手抄 */
    random_text(password, AP_PASSWORD_LEN, "abcdefghijkmnpqrstuvwxyz23456789");
    wifi_config_t config = { 0 };
    strlcpy((char *)config.ap.ssid, ssid, sizeof(config.ap.ssid));
    strlcpy((char *)config.ap.password, password, sizeof(config.ap.password));
    config.ap.ssid_len = (uint8_t)strlen(ssid);
    config.ap.channel = AP_CHANNEL;
    config.ap.max_connection = AP_MAX_CLIENTS;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start");
    s_share.wifi_started = true;
    s_share.radio = RADIO_AP;
    ESP_RETURN_ON_ERROR(http_start(), TAG, "http");

    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    film_share_status_t *st = &s_share.status;
    strlcpy(st->ssid, ssid, sizeof(st->ssid));
    strlcpy(st->password, password, sizeof(st->password));
    strlcpy(st->host, AP_IP, sizeof(st->host));
    snprintf(st->url, sizeof(st->url), "http://%s/s/%s", AP_IP, s_share.token);
    st->phase = FILM_SHARE_HOTSPOT_JOIN;
    xSemaphoreGive(s_share.lock);
    publish();
    ESP_LOGI(TAG, "hotspot %s up", ssid);
    return ESP_OK;
}

static void lan_ready(void)
{
    esp_netif_ip_info_t ip = { 0 };
    (void)esp_netif_get_ip_info(s_share.sta, &ip);
    if (http_start() != ESP_OK) {
        ESP_LOGW(TAG, "web server failed, trying hotspot");
        if (ap_start() != ESP_OK) {
            set_phase(FILM_SHARE_ERROR);
        }
        return;
    }
    s_share.radio = RADIO_LAN;
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    film_share_status_t *st = &s_share.status;
    snprintf(st->host, sizeof(st->host), IPSTR, IP2STR(&ip.ip));
    snprintf(st->url, sizeof(st->url), "http://%s/s/%s", st->host, s_share.token);
    strlcpy(st->ssid, s_share.ssid, sizeof(st->ssid));
    st->password[0] = '\0';
    st->phase = FILM_SHARE_LAN;
    xSemaphoreGive(s_share.lock);
    publish();
    ESP_LOGI(TAG, "sharing on %s", s_share.status.url);
}

/* ---------------------------------------------------------------- 分享任务 */

static void handle_command(const command_t *cmd)
{
    if (cmd->kind == CMD_END) {
        radio_off();
        xSemaphoreTake(s_share.lock, portMAX_DELAY);
        memset(&s_share.status, 0, sizeof(s_share.status));
        s_share.token[0] = '\0';
        xSemaphoreGive(s_share.lock);
        publish();
        return;
    }
    if (!cmd->hotspot && s_share.have_credentials) {
        if (sta_connect() == ESP_OK) {
            s_share.radio = RADIO_STA_CONNECTING;
            s_share.deadline_us = esp_timer_get_time() + (int64_t)STA_CONNECT_MS * 1000;
            return;
        }
        ESP_LOGW(TAG, "STA start failed, using hotspot");
    }
    if (ap_start() != ESP_OK) {
        radio_off();
        set_phase(FILM_SHARE_ERROR);
    }
}

static void share_task(void *arg)
{
    (void)arg;
    if (s_share.have_credentials && sta_connect() == ESP_OK) {
        s_share.radio = RADIO_TIME_SYNC;
        s_share.deadline_us = esp_timer_get_time() + (int64_t)TIME_SYNC_MS * 1000;
    }
    for (;;) {
        uint32_t bits = 0;
        (void)xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(TICK_MS));
        command_t cmd;
        while (xQueueReceive(s_share.commands, &cmd, 0) == pdTRUE) {
            handle_command(&cmd);
        }
        const bool timed_out = esp_timer_get_time() > s_share.deadline_us;
        switch (s_share.radio) {
        case RADIO_TIME_SYNC:
            if (atomic_load(&s_time_synced) || timed_out) {
                ESP_LOGI(TAG, "time sync %s, Wi-Fi off", atomic_load(&s_time_synced) ? "done" : "timed out");
                radio_off();
            } else if (bits & BIT_STA_LOST) {
                (void)esp_wifi_connect();
            }
            break;
        case RADIO_STA_CONNECTING:
            if (bits & BIT_GOT_IP) {
                lan_ready();
            } else if (timed_out) {
                ESP_LOGW(TAG, "router not reachable, using hotspot");
                if (ap_start() != ESP_OK) {
                    radio_off();
                    set_phase(FILM_SHARE_ERROR);
                }
            } else if (bits & BIT_STA_LOST) {
                (void)esp_wifi_connect();
            }
            break;
        case RADIO_LAN:
            if (bits & BIT_STA_LOST) {
                /* 掉线：网页暂时打不开，重连后 IP 可能变化 */
                s_share.radio = RADIO_STA_CONNECTING;
                s_share.deadline_us = esp_timer_get_time() + (int64_t)STA_CONNECT_MS * 1000;
                http_stop();
                set_phase(FILM_SHARE_STARTING);
                (void)esp_wifi_connect();
            }
            break;
        case RADIO_AP:
            if (bits & BIT_AP_JOINED) {
                xSemaphoreTake(s_share.lock, portMAX_DELAY);
                const bool changed = s_share.status.phase == FILM_SHARE_HOTSPOT_JOIN;
                if (changed) {
                    s_share.status.phase = FILM_SHARE_HOTSPOT_OPEN;
                }
                xSemaphoreGive(s_share.lock);
                if (changed) {
                    publish();
                }
            }
            break;
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------- 公共接口 */

esp_err_t film_share_begin(const uint32_t *ids, size_t count, bool force_hotspot)
{
    ESP_RETURN_ON_FALSE(s_share.initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(ids && count > 0, ESP_ERR_INVALID_ARG, TAG, "nothing to share");
    if (count > MAX_SHARE) {
        ESP_LOGW(TAG, "sharing first %d of %u photos", MAX_SHARE, (unsigned)count);
        count = MAX_SHARE;
    }
    xSemaphoreTake(s_share.lock, portMAX_DELAY);
    memcpy(s_share.ids, ids, sizeof(ids[0]) * count);
    s_share.count = count;
    s_share.downloaded = 0;
    random_text(s_share.token, TOKEN_LEN, "abcdefghijkmnpqrstuvwxyz23456789");
    memset(&s_share.status, 0, sizeof(s_share.status));
    s_share.status.phase = FILM_SHARE_STARTING;
    xSemaphoreGive(s_share.lock);
    const command_t cmd = { .kind = CMD_BEGIN, .hotspot = force_hotspot };
    ESP_RETURN_ON_FALSE(xQueueSend(s_share.commands, &cmd, 0) == pdTRUE, ESP_ERR_INVALID_STATE, TAG, "busy");
    xTaskNotify(s_share.task, BIT_COMMAND, eSetBits);
    return ESP_OK;
}

void film_share_end(void)
{
    if (!s_share.initialized) {
        return;
    }
    const command_t cmd = { .kind = CMD_END };
    (void)xQueueSend(s_share.commands, &cmd, pdMS_TO_TICKS(100));
    xTaskNotify(s_share.task, BIT_COMMAND, eSetBits);
}

esp_err_t film_share_init(const film_share_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->events, ESP_ERR_INVALID_ARG, TAG, "bad args");
    ESP_RETURN_ON_FALSE(!s_share.initialized, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    s_share.config = *config;
    s_share.lock = xSemaphoreCreateMutex();
    s_share.commands = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(command_t));
    ESP_RETURN_ON_FALSE(s_share.lock && s_share.commands, ESP_ERR_NO_MEM, TAG, "sync objects");
    s_share.have_credentials = load_credentials();
    ESP_LOGI(TAG, "saved Wi-Fi: %s", s_share.have_credentials ? s_share.ssid : "(none)");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    const esp_err_t loop = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(loop == ESP_OK || loop == ESP_ERR_INVALID_STATE, loop, TAG, "event loop");
    s_share.sta = esp_netif_create_default_wifi_sta();
    s_share.ap = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_share.sta && s_share.ap, ESP_FAIL, TAG, "netif create");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
                        TAG, "wifi events");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL,
                                                            NULL), TAG, "ip events");
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    sntp.sync_cb = on_time_sync;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&sntp), TAG, "sntp");

    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(share_task, "film_share", TASK_STACK, NULL, config->priority,
                                                &s_share.task, config->core) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task");
    s_share.initialized = true;
    return ESP_OK;
}

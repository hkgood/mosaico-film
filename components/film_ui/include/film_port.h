/*
 * film_port：界面层向平台索取的服务（相机、冲洗、存储、分享、反馈、传感器）。
 *
 * 设备（main/）和 PC 模拟器（pc/）各实现一份。界面只通过这张表与硬件打交道，
 * 因此同一份界面代码可以在模拟器里完整跑交互流程。
 *
 * 线程约定：表里所有函数都只在界面线程调用，并且必须立即返回（耗时工作交给平台自己的任务）。
 * 平台任务产生的结果通过 poll_event() 交回界面线程。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_filter.h"
#include "film_gfx.h"
#include "film_library.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 错误码沿用 esp_err_t（主机构建用 host_test/shim/esp_err.h）。界面按下列含义给出提示：
 *   ESP_ERR_INVALID_STATE  忙（上一张还在处理）
 *   ESP_ERR_NO_MEM         内存不足
 *   FILM_ERR_STORAGE_FULL  存储空间已满
 *   FILM_ERR_NO_CAMERA     相机未连接
 *   其他                   通用失败
 */
#define FILM_ERR_STORAGE_FULL ESP_ERR_INVALID_SIZE
#define FILM_ERR_NO_CAMERA    ESP_ERR_NOT_FOUND

/** 取景尺寸：M6 为 4:3 横幅，SX-70 为方形 */
#define FILM_VF_M6_W 480
#define FILM_VF_M6_H 360
#define FILM_VF_SX_W 360
#define FILM_VF_SX_H 360

/** 一帧已冲洗的取景画面（RGB565），在 preview_release 之前有效且不变 */
typedef struct {
    const uint16_t *pixels;
    uint16_t width;
    uint16_t height;
    uint32_t seq;
} film_frame_t;

/** 取景参数；instant 为真时输出方形取景 */
typedef struct {
    film_id_t film;
    float exposure_ev;
    bool instant;
} film_preview_config_t;

/** 一次拍摄（或重新冲洗）的完整参数 */
typedef struct {
    uint32_t photo_id;       /*!< 界面分配的新照片编号 */
    uint32_t source_id;      /*!< 重新冲洗时的原片编号，拍摄时为 0 */
    film_id_t film;
    float exposure_ev;
    bool instant;
    bool date_stamp;
    gfx_rot_t rot;           /*!< 握持方向：成片需要按此转正 */
    uint32_t seed;           /*!< 颗粒与漏光的随机种子 */
    bool light_leak;
    uint16_t roll;
    uint8_t frame;
    int64_t time;            /*!< 拍摄时间（Unix 秒，未知为 0） */
    uint32_t balance;        /*!< 重新冲洗时沿用原片存档的校正（film_balance_t，由 film_photo_t 拼回），拍摄时为 0 */
    /**
     * 只要屏幕尺寸预览、不存盘（重新冲洗页切换胶卷时用）。
     * film 为 FILM_ID_COUNT 时返回未冲洗的原片。
     */
    bool preview_only;
} film_shot_t;

/*
 * 一次拍摄的事件顺序（平台可以省略小样，界面对任意先后都要正确处理）：
 *   SHUTTER_DONE → DEVELOPED（屏幕尺寸小样，约 0.5 s）→ PROGRESS ×N → PROGRESS(FILM_PROGRESS_DONE)
 *   → SAVED（文件全部写完，可能在取景恢复后几秒）；任一步失败则以 SHOT_FAILED 结束。
 */
typedef enum {
    FILM_EVT_SHUTTER_DONE = 0, /*!< 快门帧已取到 */
    FILM_EVT_DEVELOPED,        /*!< 成片已冲洗：screen 为屏幕尺寸成片，需 release_result 归还 */
    FILM_EVT_SAVED,            /*!< 文件已全部写入，photo 可以加入相册 */
    FILM_EVT_SHOT_FAILED,      /*!< 拍摄或冲洗失败，err 说明原因 */
    FILM_EVT_SHARE,            /*!< 分享状态变化，内容用 share_status() 读取 */
    FILM_EVT_STORAGE,          /*!< 存储可用性变化 */
    FILM_EVT_PROGRESS,         /*!< 全尺寸冲洗进度 progress；到 FILM_PROGRESS_DONE 时取景已恢复 */
} film_event_type_t;

/** PROGRESS 事件的满值（千分比）：全尺寸冲洗完成，取景恢复，只剩后台写盘 */
#define FILM_PROGRESS_DONE 1000u

typedef struct {
    film_event_type_t type;
    esp_err_t err;
    uint32_t photo_id;        /*!< 拍摄/冲洗的新编号；重新冲洗预览时为原片编号 */
    const uint16_t *screen;   /*!< 仅 DEVELOPED：成片的屏幕图 */
    uint16_t width;
    uint16_t height;
    bool preview;             /*!< 重新冲洗的屏幕预览（不存盘） */
    film_id_t film;           /*!< 结果对应的胶卷；FILM_ID_COUNT 表示未冲洗的原片 */
    bool instant;
    uint16_t progress;        /*!< 仅 PROGRESS：0..FILM_PROGRESS_DONE */
    film_photo_t meta;       /*!< 仅 SAVED：写入相册的元数据 */
} film_event_t;

typedef enum {
    FILM_SHARE_IDLE = 0,
    FILM_SHARE_STARTING,      /*!< 正在连接 Wi-Fi 或开热点 */
    FILM_SHARE_LAN,           /*!< 与手机同一 Wi-Fi，等待打开网页 */
    FILM_SHARE_HOTSPOT_JOIN,  /*!< 热点已开，等待手机加入 */
    FILM_SHARE_HOTSPOT_OPEN,  /*!< 手机已加入热点，等待打开网页 */
    FILM_SHARE_ERROR,
} film_share_phase_t;

#define FILM_SHARE_TEXT_LEN 72

typedef struct {
    film_share_phase_t phase;
    char url[FILM_SHARE_TEXT_LEN];       /*!< 相册网页地址（含会话令牌） */
    char ssid[FILM_SHARE_TEXT_LEN];      /*!< 同网时为当前 Wi-Fi 名，热点时为热点名 */
    char password[FILM_SHARE_TEXT_LEN];  /*!< 仅热点 */
    char host[24];                        /*!< 设备 IP，用于二维码下方说明 */
    uint16_t visits;                      /*!< 网页被打开的次数 */
    uint16_t downloads;
} film_share_status_t;

typedef enum {
    FILM_FEEDBACK_SHUTTER = 0,  /*!< 快门声 + 振动 */
    FILM_FEEDBACK_DETENT,       /*!< 拨盘一格：轻触感 */
    FILM_FEEDBACK_CLICK,        /*!< 按钮 */
    FILM_FEEDBACK_LEVER,        /*!< 拨杆 */
    FILM_FEEDBACK_SHAKE,        /*!< 摇一摇换胶卷 */
    FILM_FEEDBACK_ERROR,
} film_feedback_t;

/** 电池状态 */
typedef struct {
    uint8_t percent;    /*!< 剩余电量 0..100 */
    bool charging;
} film_battery_t;

/** 屏幕状态：界面按无操作时长逐级降低，任一交互回到 ON */
typedef enum {
    FILM_DISPLAY_ON = 0,    /*!< 正常亮度 */
    FILM_DISPLAY_DIM,       /*!< 调暗，取景已停 */
    FILM_DISPLAY_OFF,       /*!< 关屏（画面为黑），声音可以关掉 */
} film_display_t;

typedef struct {
    void *ctx;

    /* 取景：取最新一帧（没有新帧返回 NULL），用完 release；config 在胶卷、曝光或机身变化时调用 */
    const film_frame_t *(*preview_acquire)(void *ctx);
    void (*preview_release)(void *ctx, const film_frame_t *frame);
    void (*preview_config)(void *ctx, const film_preview_config_t *config);
    /** 取景是否在运行（相机缺失时为 false，界面显示提示） */
    bool (*camera_ready)(void *ctx);
    /**
     * 界面暂时不需要新的取景帧（离开取景页、机身选择面板盖住取景、屏幕调暗或关闭时）：
     * 平台应立刻停掉帧转换省 CPU；暂停持续几秒后还可以关掉取景流，恢复时重新开流
     * （第一帧会晚零点几秒，界面期间一直显示手里那一帧）。可为 NULL。
     */
    void (*preview_pause)(void *ctx, bool paused);

    /* 拍摄与重新冲洗（异步，结果走事件）；忙时返回 ESP_ERR_INVALID_STATE */
    esp_err_t (*shoot)(void *ctx, const film_shot_t *shot);
    esp_err_t (*redevelop)(void *ctx, const film_shot_t *shot);
    /** 归还 DEVELOPED 事件里的 screen 缓冲 */
    void (*release_result)(void *ctx, const uint16_t *screen);

    /** 取一个平台事件，没有返回 false */
    bool (*poll_event)(void *ctx, film_event_t *ret_event);

    /* 存储：照片目录（NULL 表示存储不可用）与设置的持久化 */
    const char *(*storage_root)(void *ctx);
    bool (*settings_load)(void *ctx, void *data, size_t len);
    void (*settings_save)(void *ctx, const void *data, size_t len);

    /* 分享到手机 */
    esp_err_t (*share_start)(void *ctx, const uint32_t *ids, size_t count, bool force_hotspot);
    void (*share_stop)(void *ctx);
    void (*share_status)(void *ctx, film_share_status_t *ret_status);

    void (*feedback)(void *ctx, film_feedback_t kind);

    /** 重力方向（单位 g，已换算到屏幕坐标：x 向右、y 向下、z 朝外），不可用返回 false */
    bool (*read_accel)(void *ctx, float *x, float *y, float *z);
    /** 当前本地时间（Unix 秒），未校时返回 false */
    bool (*wall_time)(void *ctx, int64_t *ret_time);
    /** 最近一次电量读数（平台自己低频刷新并缓存，这里只取缓存），读不到返回 false。可为 NULL */
    bool (*read_battery)(void *ctx, film_battery_t *ret_battery);

    /**
     * 屏幕状态变化（创建界面时先以 ON 调用一次）：平台据此调亮度、关音频等，必须立即返回。
     * 可为 NULL。
     */
    void (*set_display)(void *ctx, film_display_t state);
    /** 平台还有不应被休眠打断的后台工作（冲洗、写盘等）时返回 true，界面就不调暗。可为 NULL */
    bool (*keep_awake)(void *ctx);
} film_port_t;

#ifdef __cplusplus
}
#endif

/*
 * film_share：把选中的照片发到手机。
 *
 *   同一 Wi-Fi：用 Vibe Mode 保存的 Wi-Fi（sysmeta/wifi，只读）连上路由器，手机扫码打开相册网页
 *   热点：连不上或用户选择时，设备自己开热点（随机密码），手机先扫码连热点、再扫码开网页
 *
 * 网页只列出这次选中的照片，地址里带一次性会话令牌；分享结束即关掉网页服务和 Wi-Fi。
 * 开机时若有保存的 Wi-Fi，会顺便连一次做网络校时（日期戳用），校时后断开省电。
 *
 * 线程：公共接口在界面线程调用、立即返回；Wi-Fi 状态机在分享任务里，网页请求在 httpd 任务里，
 * 共享状态由内部互斥量保护。状态变化时往 events 队列投递 FILM_EVT_SHARE。
 * 单例：Wi-Fi 驱动与网络接口是整机唯一资源。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    QueueHandle_t events;   /*!< film_event_t 队列 */
    int core;
    int priority;
} film_share_config_t;

esp_err_t film_share_init(const film_share_config_t *config);

/** 照片目录（存储不可用时为 NULL） */
void film_share_set_root(const char *root);

esp_err_t film_share_begin(const uint32_t *ids, size_t count, bool force_hotspot);
void film_share_end(void);
void film_share_get_status(film_share_status_t *ret_status);

/** 网络时间是否已同步过 */
bool film_share_time_synced(void);

#ifdef __cplusplus
}
#endif

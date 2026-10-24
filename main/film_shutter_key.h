// SPDX-License-Identifier: Apache-2.0
/*
 * 实体按键 → film_shell 的按键队列：
 *   - 快门：机身红色 AI 键（GPIO7）；
 *   - 自动闪光开关：USB-C 左侧的 Boot 键（GPIO61）。
 *
 * 按下、松开各投递一次，界面在渲染任务里决定拍照、收起弹层、回到取景或切换闪光。
 * AI 键只在上电那一刻由 bootloader 读取（按住上电进入 Vibe Mode），Boot 键只在上电那一刻由 ROM 读取
 * （按住上电进入下载模式），应用运行期间都可以独占使用。
 */
#pragma once

#include "esp_err.h"
#include "film_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 AI 键与 Boot 键并开始投递按键（全局单例；shell 须在整个运行期有效，不再停止）
 *
 * Boot 键创建失败只记一条警告（闪光改用设置页切换），不影响快门。
 *
 * @param shell 已启动的界面外壳
 * @return ESP_OK；ESP_ERR_INVALID_ARG（shell 为空）；ESP_ERR_INVALID_STATE（已启动）；或 AI 键的按键驱动错误
 */
esp_err_t film_shutter_key_start(film_shell_handle_t shell);

#ifdef __cplusplus
}
#endif

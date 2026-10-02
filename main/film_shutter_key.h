// SPDX-License-Identifier: Apache-2.0
/*
 * 实体快门键：机身红色 AI 键（GPIO7）→ film_shell 的按键队列。
 *
 * 按下、松开各投递一次，界面在渲染任务里决定拍照、收起弹层还是回到取景。
 * AI 键只在上电那一刻由 bootloader 读取（按住上电进入 Vibe Mode），应用运行期间可以独占使用。
 */
#pragma once

#include "esp_err.h"
#include "film_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 AI 键并开始投递按键（全局单例；shell 须在整个运行期有效，不再停止）
 *
 * @param shell 已启动的界面外壳
 * @return ESP_OK；ESP_ERR_INVALID_ARG（shell 为空）；ESP_ERR_INVALID_STATE（已启动）；或按键驱动错误
 */
esp_err_t film_shutter_key_start(film_shell_handle_t shell);

#ifdef __cplusplus
}
#endif

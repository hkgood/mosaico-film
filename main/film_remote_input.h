// SPDX-License-Identifier: Apache-2.0
/*
 * 远程触摸：Gateway 的 pointer/v1 RPC → film_shell 的触摸队列。
 *
 * 让开发者在 Gateway 网页（或脚本）里直接点按相机界面，与真实触摸进同一个 film_app_pointer。
 * 只接受已就绪的 USB 会话，网络侧的 Iris 连接不能远程操作界面。
 */
#pragma once

#include "esp_err.h"
#include "film_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 注册指针 RPC（全局单例；shell 须在整个运行期有效，不再停止）
 *
 * @param shell 已启动的界面外壳
 * @return ESP_OK；ESP_ERR_INVALID_ARG（shell 为空）；ESP_ERR_INVALID_STATE（已注册）；或 RPC 注册错误
 */
esp_err_t film_remote_input_start(film_shell_handle_t shell);

#ifdef __cplusplus
}
#endif

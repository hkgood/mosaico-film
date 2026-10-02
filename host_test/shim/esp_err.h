/*
 * 主机测试用的最小 esp_err.h：只提供 film_filter 等可移植组件用到的错误码，
 * 数值与 ESP-IDF 保持一致。设备构建使用 ESP-IDF 自带的头文件。
 */
#pragma once

#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                -1
#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107
#define ESP_ERR_INVALID_VERSION 0x10A

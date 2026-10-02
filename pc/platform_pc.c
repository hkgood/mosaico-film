// SPDX-License-Identifier: Apache-2.0
/*
 * PC 模拟器入口：创建模拟平台服务，把 film_app 挂到 sim_bridge 的 GSP 句柄上。
 * 与设备共用 main/film_shell.c；这里只替换平台服务（film_port_pc.c）。
 */
#include <stdio.h>
#include <stdlib.h>

/* Canvas 接口按 bind 编号寻址，需要场景的原始编号 */
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "film_port_pc.h"
#include "film_shell.h"
#include "gsp_sim_bridge.h"

static film_port_pc_handle_t s_port;
static film_shell_handle_t s_shell;

static uint32_t sim_now_ms(void)
{
    return (uint32_t)gsp_sim_bridge_time_ms();
}

static void *sim_alloc(size_t bytes)
{
    return malloc(bytes);
}

esp_gsp_err_t gsp_bridge_app_init(esp_gsp_handle_t ui)
{
    const char *data_dir = getenv("FILM_SIM_DATA_DIR");
    const film_port_pc_config_t port_config = {
        .data_dir = data_dir ? data_dir : FILM_SIM_DATA_DIR,
        .sensor_ppm = FILM_SIM_SENSOR_PPM,
        .assets_path = FILM_SIM_ASSETS,
    };
    if (film_port_pc_create(&port_config, &s_port) != ESP_OK) {
        return ESP_GSP_FAIL;
    }
    const film_shell_config_t shell_config = {
        .canvas_bind = GSP_FILM_BIND_SCREEN,
        .port = film_port_pc_table(s_port),
        .seed = (uint32_t)sim_now_ms() | 1u,
        .now_ms = sim_now_ms,
        .alloc = sim_alloc,
        .free = free,
    };
    esp_gsp_err_t err = film_shell_start(ui, &shell_config, &s_shell);
    if (err == ESP_GSP_OK) {
        err = gsp_sim_bridge_poll(ui, 0);
    }
    return err;
}

void gsp_bridge_app_deinit(esp_gsp_handle_t ui)
{
    film_shell_stop(ui, s_shell);
    s_shell = NULL;
    film_port_pc_delete(s_port);
    s_port = NULL;
    fprintf(stderr, "mosaico_film sim: stopped\n");
}

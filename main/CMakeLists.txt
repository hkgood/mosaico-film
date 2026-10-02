idf_component_register(
    SRCS
        "main.c"
        "board_display.c"
        "film_shell.c"
        "film_boot.c"
        "film_port_dev.c"
        "film_power.c"
        "film_discharge.c"
        "film_display.c"
        "film_camera.c"
        "film_still.c"
        "film_lab.c"
        "film_writer.c"
        "film_storage.c"
        "film_feedback.c"
        "film_share.c"
        "film_remote_input.c"
        "film_shutter_key.c"
    INCLUDE_DIRS "."
    REQUIRES esp_iris esp-mosaico-bsp esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris freertos nvs_flash
        esp-gsp film_ui
    PRIV_REQUIRES esp_timer esp_app_format esp_display_present esp_lcd esp_partition
        film_filter film_gfx film_assets film_darkroom
        mosaico_module_camera mosaico_module_mgr esp_driver_jpeg esp_driver_gpio esp_driver_spi esp_mm
        espressif__esp_video espressif__esp_cam_sensor espressif__esp_new_jpeg
        espressif__spi_nand_flash joltwallet__littlefs
        esp_wifi esp_netif esp_event esp_http_server espressif__button)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_bundle.cmake")
mosaico_gsp_add_ui_bundle(${COMPONENT_LIB} "../ui/main.json")

# 字体、贴图与开机动画放在 assets 分区（运行时映射），随应用的分区布局一起经 system-update 写入
set(film_assets_image "${CMAKE_CURRENT_LIST_DIR}/../components/film_assets/assets.bin")
add_custom_target(film_assets_image DEPENDS "${film_assets_image}")
set_property(GLOBAL APPEND PROPERTY MOSAICO_SYSTEM_UPDATE_DATA_LABELS assets)
set_property(GLOBAL PROPERTY MOSAICO_SYSTEM_UPDATE_DATA_assets_IMAGE "${film_assets_image}")
set_property(GLOBAL PROPERTY MOSAICO_SYSTEM_UPDATE_DATA_assets_TARGET film_assets_image)

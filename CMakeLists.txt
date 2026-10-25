cmake_minimum_required(VERSION 3.16)
set(MOSAICO_UTILS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-utils")
set(MOSAICO_BSP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../submodule/esp-mosaico-bsp")
if(NOT SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
if(NOT SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.application.defaults")
endif()
# The BSP sends LCD chunks from PSRAM through internal DMA bounce buffers (~75 KB
# held while frames are queued), which leaves no room for the share Wi-Fi driver.
# This patch adds CONFIG_BSP_LCD_PSRAM_DMA_DIRECT; drop it once the BSP ships one.
# Failing to apply must stop the build: silently losing it brings back NO_MEM.
set(FILM_BSP_PATCH "${CMAKE_CURRENT_LIST_DIR}/patches/esp-mosaico-bsp-lcd-psram-dma-direct.patch")
find_program(FILM_GIT git REQUIRED)
execute_process(
    COMMAND "${FILM_GIT}" -C "${MOSAICO_BSP_ROOT}" apply --reverse --check "${FILM_BSP_PATCH}"
    RESULT_VARIABLE FILM_BSP_PATCH_APPLIED OUTPUT_QUIET ERROR_QUIET)
if(NOT FILM_BSP_PATCH_APPLIED EQUAL 0)
    execute_process(
        COMMAND "${FILM_GIT}" -C "${MOSAICO_BSP_ROOT}" apply "${FILM_BSP_PATCH}"
        RESULT_VARIABLE FILM_BSP_PATCH_RESULT ERROR_VARIABLE FILM_BSP_PATCH_ERROR)
    if(NOT FILM_BSP_PATCH_RESULT EQUAL 0)
        message(FATAL_ERROR "mosaico_film: BSP patch ${FILM_BSP_PATCH} no longer applies; "
                            "port it to the current BSP: ${FILM_BSP_PATCH_ERROR}")
    endif()
    message(STATUS "mosaico_film: applied BSP LCD PSRAM direct DMA patch")
endif()
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/gsp_compiler.cmake")
list(APPEND EXTRA_COMPONENT_DIRS
    "${MOSAICO_UTILS_ROOT}/esp-mosaico-vibe-mode/components/esp_mosaico_app_recovery"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_bundle"
    "${MOSAICO_UTILS_ROOT}/mosaico-tools/components/esp_mosaico_gsp_iris")
include("${MOSAICO_UTILS_ROOT}/esp-mosaico-vibe-mode/cmake/mosaico_idf_project.cmake")
project(mosaico_film VERSION 1.3.0)
include("${MOSAICO_UTILS_ROOT}/mosaico-tools/cmake/system_update.cmake")

cmake_minimum_required(VERSION 3.20)
project(mosaico_film_backend LANGUAGES C)

if(NOT ESP_GSP_COMPONENT_DIR AND DEFINED ENV{ESP_GSP_COMPONENT_DIR})
    set(ESP_GSP_COMPONENT_DIR "$ENV{ESP_GSP_COMPONENT_DIR}")
endif()
if(NOT ESP_GSP_COMPONENT_DIR)
    foreach(candidate IN ITEMS
            "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp-gsp")
        get_filename_component(candidate "${candidate}" ABSOLUTE)
        if(EXISTS "${candidate}/tools/sim_bridge/CMakeLists.txt")
            set(ESP_GSP_COMPONENT_DIR "${candidate}")
            break()
        endif()
    endforeach()
endif()
if(NOT EXISTS "${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge/CMakeLists.txt")
    message(FATAL_ERROR
        "Install espressif/esp-gsp first, or set ESP_GSP_COMPONENT_DIR")
endif()

set(APP_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(COMP "${APP_DIR}/components")
file(GLOB FILM_UI_SOURCES "${COMP}/film_ui/*.c")

add_subdirectory("${ESP_GSP_COMPONENT_DIR}/tools/sim_bridge" sim_bridge)
gsp_add_backend(mosaico_film_backend
    SOURCES
        platform_pc.c
        film_port_pc.c
        ../main/film_shell.c
        ../host_test/ppm_io.c
        ${FILM_UI_SOURCES}
        ${COMP}/film_gfx/film_gfx.c
        ${COMP}/film_assets/assets_table.c
        ${COMP}/qrcodegen/qrcodegen.c
        ${COMP}/film_darkroom/film_balance.c
        ${COMP}/film_darkroom/film_darkroom.c
        ${COMP}/film_darkroom/film_studio.c
        ${COMP}/film_filter/film_filter.c
        ${COMP}/film_filter/film_color.c
        ${COMP}/film_filter/film_halation.c
        ${COMP}/film_filter/film_pixel.c
        ${COMP}/film_filter/film_presets.c
        ${COMP}/film_filter/port/film_worker_pthread.c
    INCLUDE_DIRECTORIES
        ../main
        ../host_test
        ../host_test/shim
        ${COMP}/film_ui
        ${COMP}/film_ui/include
        ${COMP}/film_gfx/include
        ${COMP}/film_assets/include
        ${COMP}/qrcodegen/include
        ${COMP}/film_darkroom/include
        ${COMP}/film_filter
        ${COMP}/film_filter/include
    SCENES ../ui/main.json)

target_compile_definitions(mosaico_film_backend PRIVATE
    FILM_SIM_DATA_DIR="${CMAKE_BINARY_DIR}/sim_data"
    FILM_SIM_SENSOR_PPM="${CMAKE_CURRENT_LIST_DIR}/assets/sensor_mock.ppm"
    FILM_SIM_ASSETS="${COMP}/film_assets/assets.bin")
find_package(Threads REQUIRED)
target_link_libraries(mosaico_film_backend PRIVATE Threads::Threads m)

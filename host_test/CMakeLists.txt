# 在主机上编译可移植组件，运行单元测试，并提供与 Python 原型对比用的命令行工具。
#   cmake -S host_test -B build_host && cmake --build build_host && ctest --test-dir build_host
cmake_minimum_required(VERSION 3.16)
project(mosaico_film_host_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(FILM_SANITIZE "Build with AddressSanitizer and UBSan" OFF)
if(FILM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

set(FILM_FILTER_DIR "${CMAKE_CURRENT_LIST_DIR}/../components/film_filter")

add_library(film_filter STATIC
    "${FILM_FILTER_DIR}/film_filter.c"
    "${FILM_FILTER_DIR}/film_color.c"
    "${FILM_FILTER_DIR}/film_halation.c"
    "${FILM_FILTER_DIR}/film_pixel.c"
    "${FILM_FILTER_DIR}/film_presets.c"
    "${FILM_FILTER_DIR}/port/film_worker_pthread.c")
target_include_directories(film_filter
    PUBLIC "${FILM_FILTER_DIR}/include" "${CMAKE_CURRENT_LIST_DIR}/shim"
    PRIVATE "${FILM_FILTER_DIR}")
target_compile_options(film_filter PRIVATE -Wall -Wextra -Werror -Wshadow -Wconversion -Wno-sign-conversion)
find_package(Threads REQUIRED)
target_link_libraries(film_filter PUBLIC Threads::Threads)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(film_filter PUBLIC ${MATH_LIBRARY})
endif()

add_executable(film_cli film_cli.c ppm_io.c)
target_link_libraries(film_cli PRIVATE film_filter)
target_compile_options(film_cli PRIVATE -Wall -Wextra -Werror)

add_executable(film_filter_test test_film_filter.c)
target_link_libraries(film_filter_test PRIVATE film_filter)
target_compile_options(film_filter_test PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_test(NAME film_filter_test COMMAND film_filter_test)

# ---------------------------------------------------------------- 界面层（绘图、素材、二维码、film_ui）
set(COMPONENTS_DIR "${CMAKE_CURRENT_LIST_DIR}/../components")
set(FILM_UI_WARNINGS -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter)

add_library(film_gfx STATIC "${COMPONENTS_DIR}/film_gfx/film_gfx.c")
target_include_directories(film_gfx PUBLIC "${COMPONENTS_DIR}/film_gfx/include")
target_compile_options(film_gfx PRIVATE ${FILM_UI_WARNINGS})
if(MATH_LIBRARY)
    target_link_libraries(film_gfx PUBLIC ${MATH_LIBRARY})
endif()

add_library(film_assets STATIC "${COMPONENTS_DIR}/film_assets/assets_table.c")
target_include_directories(film_assets PUBLIC "${COMPONENTS_DIR}/film_assets/include" "${CMAKE_CURRENT_LIST_DIR}/shim")
target_link_libraries(film_assets PUBLIC film_gfx)

add_library(qrcodegen STATIC "${COMPONENTS_DIR}/qrcodegen/qrcodegen.c")
target_include_directories(qrcodegen PUBLIC "${COMPONENTS_DIR}/qrcodegen/include")

file(GLOB FILM_UI_SOURCES "${COMPONENTS_DIR}/film_ui/*.c")
add_library(film_ui STATIC ${FILM_UI_SOURCES})
target_include_directories(film_ui PUBLIC "${COMPONENTS_DIR}/film_ui/include" PRIVATE "${COMPONENTS_DIR}/film_ui")
target_link_libraries(film_ui PUBLIC film_filter film_gfx film_assets qrcodegen)
target_compile_options(film_ui PRIVATE ${FILM_UI_WARNINGS})

# ---------------------------------------------------------------- 暗房（成片管线）
add_library(film_darkroom STATIC
    "${COMPONENTS_DIR}/film_darkroom/film_balance.c"
    "${COMPONENTS_DIR}/film_darkroom/film_darkroom.c"
    "${COMPONENTS_DIR}/film_darkroom/film_studio.c"
    "${COMPONENTS_DIR}/film_darkroom/film_viewfinder.c")
target_include_directories(film_darkroom PUBLIC "${COMPONENTS_DIR}/film_darkroom/include")
target_link_libraries(film_darkroom PUBLIC film_filter film_gfx film_assets film_ui)
target_compile_options(film_darkroom PRIVATE ${FILM_UI_WARNINGS})

add_executable(film_darkroom_test test_film_darkroom.c ppm_io.c)
target_link_libraries(film_darkroom_test PRIVATE film_darkroom)
target_compile_options(film_darkroom_test PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(film_darkroom_test PRIVATE
    FILM_TEST_ASSETS="${COMPONENTS_DIR}/film_assets/assets.bin"
    FILM_TEST_SENSOR="${CMAKE_CURRENT_LIST_DIR}/../pc/assets/sensor_mock.ppm")
add_test(NAME film_darkroom_test COMMAND film_darkroom_test "${CMAKE_CURRENT_BINARY_DIR}/darkroom_out")

add_executable(film_library_test test_film_library.c)
target_link_libraries(film_library_test PRIVATE film_ui)
target_compile_options(film_library_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME film_library_test COMMAND film_library_test)

# ---------------------------------------------------------------- 界面交互脚本（PC 平台服务 + 完整 film_app）
add_executable(film_ui_script film_ui_script.c ppm_io.c "${CMAKE_CURRENT_LIST_DIR}/../pc/film_port_pc.c")
# 宣传片素材录制（tools/promo）：与交互脚本同样驱动完整的 film_app，不作为测试运行
add_executable(film_promo_capture "${CMAKE_CURRENT_LIST_DIR}/../tools/promo/promo_capture.c" ppm_io.c
    "${CMAKE_CURRENT_LIST_DIR}/../pc/film_port_pc.c")
target_include_directories(film_promo_capture PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../pc" "${CMAKE_CURRENT_LIST_DIR}"
    "${COMPONENTS_DIR}/film_ui")
target_link_libraries(film_promo_capture PRIVATE film_ui film_darkroom)
target_compile_options(film_promo_capture PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(film_promo_capture PRIVATE FILM_TEST_ASSETS="${COMPONENTS_DIR}/film_assets/assets.bin")

# 脚本要断言页面与弹层状态，所以额外看得到 film_ui 的私有头
target_include_directories(film_ui_script PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../pc" "${CMAKE_CURRENT_LIST_DIR}"
    "${COMPONENTS_DIR}/film_ui")
target_link_libraries(film_ui_script PRIVATE film_ui film_darkroom)
target_compile_options(film_ui_script PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(film_ui_script PRIVATE
    FILM_TEST_ASSETS="${COMPONENTS_DIR}/film_assets/assets.bin"
    FILM_TEST_SENSOR="${CMAKE_CURRENT_LIST_DIR}/../pc/assets/sensor_mock.ppm")

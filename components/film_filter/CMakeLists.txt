idf_component_register(
    SRCS "film_filter.c" "film_color.c" "film_halation.c" "film_pixel.c" "film_presets.c"
         "port/film_worker_freertos.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    PRIV_REQUIRES freertos)

# 逐像素热循环按性能优化编译，不受工程整体的 -Os 影响
set_source_files_properties("film_color.c" "film_pixel.c" PROPERTIES COMPILE_OPTIONS "-O2")

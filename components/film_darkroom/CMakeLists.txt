# 成片冲洗（裁切转正、胶卷滤镜、宝丽来相框、日期戳）与写盘流程，设备与 PC 模拟器共用。
idf_component_register(
    SRCS "film_balance.c" "film_darkroom.c" "film_studio.c" "film_viewfinder.c"
    INCLUDE_DIRS "include"
    REQUIRES film_filter film_gfx film_ui
    PRIV_REQUIRES film_assets)

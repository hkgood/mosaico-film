# 素材表（字体、贴图在 assets.bin 中的位置）。assets.bin 本身经 system-update 写入 assets 分区，
# 运行时映射后用 film_assets_bind() 绑定，不进固件。
idf_component_register(
    SRCS "assets_table.c"
    INCLUDE_DIRS "include"
    REQUIRES film_gfx)

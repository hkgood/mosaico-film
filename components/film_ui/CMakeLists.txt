# 相机界面（M6 / SX-70 机身、相册、重新冲洗、分享、胶卷选择）。设备与 PC 模拟器共用，只经 film_port 访问平台。
idf_component_register(
    SRCS
        "app.c"
        "app_power.c"
        "cam_picker.c"
        "cam_sx.c"
        "film_library.c"
        "scr_album.c"
        "scr_camera.c"
        "scr_detail.c"
        "scr_film.c"
        "scr_redevelop.c"
        "scr_share.c"
        "ui_draw.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES film_filter film_gfx
    PRIV_REQUIRES film_assets qrcodegen)

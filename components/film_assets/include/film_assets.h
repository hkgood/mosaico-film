/* 由 tools/assets/make_assets.py 生成，请勿手改。 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "film_gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 素材在 film_assets_bind() 之前不可用（位图指针为 NULL）。
 * 绑定后各结构体只读；素材数据的内存（设备上为 Flash 映射）必须在整个运行期间有效。
 */
#define FILM_ASSETS_LAYOUT_ID 0xbc328cc7u
#define FILM_ASSETS_SIZE 4847024u

/** 检查素材数据的版本与布局，并把全部字体、图片指向其中 */
esp_err_t film_assets_bind(const void *data, size_t size);
/** 开机动画（MFB1 格式），未绑定时返回 false */
bool film_assets_boot_anim(const uint8_t **ret_data, size_t *ret_size);

extern gfx_font_t font_jost_m8;
extern gfx_font_t font_jost_m9;
extern gfx_font_t font_jost_m10;
extern gfx_font_t font_jost_m11;
extern gfx_font_t font_jost_m12;
extern gfx_font_t font_jost_m14;
extern gfx_font_t font_jost_m18;
extern gfx_font_t font_jost_m19;
extern gfx_font_t font_jost_m22;
extern gfx_font_t font_jost_m25;
extern gfx_font_t font_jost_r11;
extern gfx_font_t font_jost_r12;
extern gfx_font_t font_jost_r16;
extern gfx_font_t font_jost_m11_glow;
extern gfx_font_t font_jost_m19_glow;
extern gfx_font_t font_jost_m25_glow;
extern gfx_font_t font_dseg_20;
extern gfx_font_t font_dseg_20_glow;
extern gfx_font_t font_dseg_60;
extern gfx_font_t font_dseg_60_glow;
extern gfx_font_t font_noto_11;
extern gfx_font_t font_noto_12;
extern gfx_font_t font_noto_13;

/* 精灵图；*_OX/_OY 为位图左上角相对元素框左上角的偏移（含阴影外扩） */
extern gfx_image_t img_counter56;
#define IMG_COUNTER56_OX (-6)
#define IMG_COUNTER56_OY (-6)
extern gfx_image_t img_counter72;
#define IMG_COUNTER72_OX (-8)
#define IMG_COUNTER72_OY (-8)
extern gfx_image_t img_shutter;
#define IMG_SHUTTER_OX (-10)
#define IMG_SHUTTER_OY (-10)
extern gfx_image_t img_shutter_down;
#define IMG_SHUTTER_DOWN_OX (-10)
#define IMG_SHUTTER_DOWN_OY (-10)
extern gfx_image_t img_win140_base;
#define IMG_WIN140_BASE_OX (-8)
#define IMG_WIN140_BASE_OY (-8)
extern gfx_image_t img_win140_over;
#define IMG_WIN140_OVER_OX (-8)
#define IMG_WIN140_OVER_OY (-8)
extern gfx_image_t img_win140_glow;
#define IMG_WIN140_GLOW_OX (-16)
#define IMG_WIN140_GLOW_OY (-16)
extern gfx_image_t img_lever_off;
#define IMG_LEVER_OFF_OX (-4)
#define IMG_LEVER_OFF_OY (-4)
extern gfx_image_t img_lever_on;
#define IMG_LEVER_ON_OX (-4)
#define IMG_LEVER_ON_OY (-4)
extern gfx_image_t img_popover;
#define IMG_POPOVER_OX (-34)
#define IMG_POPOVER_OY (-34)
extern gfx_image_t img_rollend;
#define IMG_ROLLEND_OX (-44)
#define IMG_ROLLEND_OY (-44)
extern gfx_image_t img_sx_shutter;
#define IMG_SX_SHUTTER_OX (-8)
#define IMG_SX_SHUTTER_OY (-8)
extern gfx_image_t img_sx_shutter_down;
#define IMG_SX_SHUTTER_DOWN_OX (-8)
#define IMG_SX_SHUTTER_DOWN_OY (-8)
extern gfx_image_t img_pack;
#define IMG_PACK_OX (-8)
#define IMG_PACK_OY (-8)
extern gfx_image_t img_m6_plate120;
extern gfx_image_t img_m6_plate180;
extern gfx_image_t img_icon_back;
extern gfx_image_t img_icon_close;
extern gfx_image_t img_icon_trash;
extern gfx_image_t img_icon_phone;
extern gfx_image_t img_icon_check;
extern gfx_image_t img_icon_wifi;
extern gfx_image_t img_sx_body;
extern gfx_image_t img_dev_bg;
extern gfx_image_t img_dev_paper;
#define IMG_DEV_PAPER_OX (76)
#define IMG_DEV_PAPER_OY (22)
extern gfx_image_t img_picker_m6;
extern gfx_image_t img_picker_sx70;
extern gfx_image_t img_can_gold;
extern gfx_image_t img_can_gold_s;
extern gfx_image_t img_can_soft;
extern gfx_image_t img_can_soft_s;
extern gfx_image_t img_can_verde;
extern gfx_image_t img_can_verde_s;
extern gfx_image_t img_can_cross;
extern gfx_image_t img_can_cross_s;
extern gfx_image_t img_can_silver;
extern gfx_image_t img_can_silver_s;
extern gfx_image_t img_can_faded;
extern gfx_image_t img_can_faded_s;
extern gfx_image_t img_can_night;
extern gfx_image_t img_can_night_s;
extern gfx_image_t img_can_pixel;
extern gfx_image_t img_can_pixel_s;
extern gfx_image_t img_tex_alu;
extern gfx_image_t img_tex_vulc;
extern gfx_image_t img_tex_paper;
extern gfx_image_t img_tex_leather;

#ifdef __cplusplus
}
#endif

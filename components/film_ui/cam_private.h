/*
 * 取景页内部接口：scr_camera.c（公共逻辑 + M6 机身）与 cam_sx.c（SX-70 机身）之间共享。
 */
#pragma once

#include "app_private.h"

/* M6 机身布局（屏幕坐标，取自 v3 样稿；底行左右两端收进下方圆角安全区） */
#define M6_PLATE_Y        360
#define M6_COUNTER_X      92
#define M6_COUNTER_Y      410
#define M6_SHUTTER_X      240
#define M6_SHUTTER_Y      414
#define M6_WINDOW_X       378
#define M6_WINDOW_Y       410
#define M6_WINDOW_W       140
#define M6_WINDOW_H       38
#define M6_LABEL_GAP      42      /*!< 部件中心到下方刻字（ROLL / FILM）的距离 */
#define M6_LABEL_GAP_V    12      /*!< 竖拿时胶卷窗侧边到 FILM 刻字的距离 */

/* SX-70 机身布局 */
#define SX_VF_X           60
#define SX_COUNTER_X      452     /*!< 计数窗中心：右侧皮面只有 60 宽，只能下移避开右上圆角 */
#define SX_COUNTER_Y      90
#define SX_STACK_X        82
#define SX_STACK_Y        424
#define SX_SHUTTER_X      240
#define SX_SHUTTER_Y      426
#define SX_PACK_X         396
#define SX_PACK_Y         416
#define SX_PRINT_SIZE     CAM_STACK_PRINT_PX

/* 弹出面板的位置（以样稿 M6 为准，SX-70 按胶片盒位置平移） */
#define POPOVER_Y         250
#define POPOVER_W         184
#define POPOVER_H         94
#define POPOVER_CARET_DX  128

/** 0..1 的缓动 */
float cam_ease_out(float t);
float cam_ease_in_out(float t);
float cam_progress(uint32_t now, uint32_t t0, uint32_t duration);

/* 公共动作 */
void cam_fire(film_app_t *app);
void cam_set_ev(film_app_t *app, float ev);
void cam_open(film_app_t *app, cam_overlay_t overlay);
void cam_close(film_app_t *app);
void cam_toggle_instant(film_app_t *app);
/** 换到指定机身（皮革合拢动画）；已是该机身时什么也不做 */
void cam_switch_body(film_app_t *app, bool instant);
void cam_load_new_roll(film_app_t *app, bool change_film);

/* 公共绘制 */
/** 取景画面：live 帧（按 dst 尺寸居中裁剪）或相机缺失提示 */
void cam_draw_live(film_app_t *app, gfx_canvas_t *c, gfx_rect_t dst);
/** 取景叠加：曝光刻度、日期戳；M6 另加取景框、测距黄斑、水平仪 */
void cam_draw_vf_overlay(film_app_t *app, gfx_canvas_t *c, gfx_rect_t vf, bool rangefinder);
void cam_draw_popover(film_app_t *app, gfx_canvas_t *c, int x);
void cam_draw_rollend(film_app_t *app, gfx_canvas_t *c);
void cam_draw_skin_swap(film_app_t *app, gfx_canvas_t *c);
/** 弹出面板与一卷拍完的手势（两种机身共用），返回是否已处理 */
bool cam_overlay_gesture(film_app_t *app, const gesture_t *g, int popover_x);

/* 机身选择面板（cam_picker.c）：从屏幕顶边下拉打开，两种机身共用 */
/** 处理下拉、上推与卡片点击；返回 true 表示手势已被面板消费 */
bool cam_picker_gesture(film_app_t *app, const gesture_t *g);
/** 推进展开/收起动画；返回是否需要重绘 */
bool cam_picker_step(film_app_t *app, uint32_t dt_ms);
/** 画顶边的下拉提示条和（展开中的）面板，盖在机身之上 */
void cam_picker_render(film_app_t *app, gfx_canvas_t *c);
/** 面板不透明地盖住了屏幕顶部多少行（收起时为 0）；这些行里的机身不必画 */
int cam_picker_covered_rows(const cam_state_t *cam);
void cam_picker_reset(film_app_t *app);

/* SX-70 */
void cam_sx_render(film_app_t *app, gfx_canvas_t *c);
void cam_sx_gesture(film_app_t *app, const gesture_t *g);
bool cam_sx_step(film_app_t *app);
void cam_sx_event(film_app_t *app, const film_event_t *ev);
void cam_sx_refresh_stack(film_app_t *app);

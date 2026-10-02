/*
 * film_app 内部共享定义：应用状态、各页面的接口表、手势与绘制辅助。
 *
 * 所有状态由界面线程独占；页面之间只通过 app_* 函数切换与共享数据。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "film_app.h"
#include "film_assets.h"
#include "film_gfx.h"
#include "film_library.h"
#include "film_port.h"

#define SCREEN_W FILM_APP_SCREEN_W
#define SCREEN_H FILM_APP_SCREEN_H

/* ---------------------------------------------------------------- 配色（与 v3 样稿一致） */
#define COLOR_INK       0x0E0E0DU
#define COLOR_CREAM     0xF1EBDCU
#define COLOR_MUTED     0x8D877BU
#define COLOR_AMBER     0xFF9F2EU
#define COLOR_LED       0xFF3B2BU
#define COLOR_ENGRAVE   0x262624U
#define COLOR_ENGRAVE_LT 0xE6DCCBU
#define COLOR_TAGLINE   0x5A5854U
#define COLOR_STAMP     0xFF8A1FU
#define COLOR_WHITE     0xFFFFFFU
#define COLOR_BLACK     0x000000U
#define COLOR_PAPER     0xF3EEE3U
#define COLOR_UNDEVELOPED 0x2B322CU   /*!< 显影初期的灰绿色 */

/* ---------------------------------------------------------------- 屏幕圆角安全区
 * 屏幕四角是圆角（物理半径约 48 px）。可点击的控件与文字信息都放进
 * "四边内缩 SAFE_INSET、四角圆弧半径 SAFE_RADIUS"的圆角矩形里，留出余量；
 * 照片、皮纹、铝板等背景照常铺满全屏。
 */
#define SAFE_INSET           12
#define SAFE_RADIUS          64

/* 暗房页眉（相册、大图、重新冲洗共用）：返回键收进左上圆弧以内 */
#define HEADER_H             64
#define HEADER_BACK_X        36      /*!< 返回键（直径 36 的圆）左上角 */
#define HEADER_BACK_Y        22
#define HEADER_TEXT_X        86      /*!< 标题、副标题左边 */
#define HEADER_TITLE_Y       31
#define HEADER_SUB_Y         48
#define HEADER_RIGHT_X       440     /*!< 右侧文字（SELECT、页码等）右对齐到这里 */
#define HEADER_RIGHT_Y       40
#define BACK_HIT_W           96      /*!< 返回键点击区：左上 96×76，远大于图标，好按 */
#define BACK_HIT_H           76
#define HEADER_RIGHT_HIT_W   140     /*!< 右侧文字的点击区宽度（从右边缘算） */

/* 底栏按钮行（大图、分享、多选共用）：两端收进下方圆弧以内 */
#define BOTTOM_ROW_Y         398
#define BOTTOM_ROW_H         48
#define BOTTOM_ROW_LEFT      36
#define BOTTOM_ROW_RIGHT     444
#define BOTTOM_ROW_GAP       12

/* ---------------------------------------------------------------- 胶卷与时长 */
#define ROLL_FRAMES          36
#define SELECT_MAX           20
#define EV_STEP              (1.0f / 3.0f)
#define EV_PX_PER_STOP       80      /*!< 上下滑动多少像素改变 1 EV */
#define FILM_SWIPE_PX        70      /*!< 横向滑动多少像素换一卷 */
#define TAP_SLOP_PX          12
#define LONG_PRESS_MS        480
#define DOUBLE_TAP_MS        320
#define TOAST_MS             1800
#define EV_SCALE_LINGER_MS   1400
#define DEVELOP_LINGER_MS    1600    /*!< SX-70 显影完成后停留多久自动回取景 */
#define SKIN_SWAP_MS         260     /*!< 皮革合拢/打开各用时 */
#define FRAME_PERIOD_MS      33

/* ---------------------------------------------------------------- 电量 */
#define BATTERY_POLL_MS          1000    /*!< 向平台取缓存读数的间隔（平台自己更低频地读电量计） */
#define BATTERY_LOW_PERCENT      20      /*!< 不高于此值显示琥珀色 */
#define BATTERY_CRITICAL_PERCENT 10      /*!< 不高于此值显示红色，并提示一次 */
#define BATTERY_REARM_PERCENT    15      /*!< 回升到此值以上（或开始充电）后，下次降到 10% 再提示 */

/* ---------------------------------------------------------------- 页面 */
typedef enum {
    SCR_CAMERA = 0,
    SCR_ALBUM,
    SCR_DETAIL,
    SCR_REDEVELOP,
    SCR_SHARE,
    SCR_COUNT,
} screen_id_t;

/** 识别后的手势；坐标为屏幕坐标，du/dv 为换算到"用户方向"后的位移 */
typedef enum {
    GEST_PRESS = 0,     /*!< 手指按下 */
    GEST_TAP,           /*!< 轻点（抬起时触发） */
    GEST_DOUBLE_TAP,    /*!< 第二次轻点（在 TAP 之后额外触发） */
    GEST_LONG,          /*!< 按住不动超过 LONG_PRESS_MS */
    GEST_DRAG_BEGIN,
    GEST_DRAG,
    GEST_DRAG_END,
    GEST_RELEASE,       /*!< 任何手势结束时都会触发，用于恢复按下态 */
} gesture_kind_t;

typedef struct {
    gesture_kind_t kind;
    int16_t x, y;       /*!< 当前位置 */
    int16_t x0, y0;     /*!< 按下位置 */
    int16_t dx, dy;     /*!< 相对按下位置的屏幕位移 */
    int16_t du, dv;     /*!< 相对按下位置的用户方向位移 */
    float vx, vy;       /*!< 抬起时的屏幕速度（像素/秒） */
    bool horizontal;    /*!< 拖动方向（用户方向）：true 横向 */
} gesture_t;

typedef struct film_app_t film_app_t;

typedef struct {
    void (*enter)(film_app_t *app);
    void (*leave)(film_app_t *app);
    /** 每帧推进动画；返回是否需要重绘 */
    bool (*step)(film_app_t *app, uint32_t dt_ms);
    void (*render)(film_app_t *app, gfx_canvas_t *c);
    void (*gesture)(film_app_t *app, const gesture_t *g);
    void (*event)(film_app_t *app, const film_event_t *ev);
    /** 实体按键；NULL 表示本页忽略按键 */
    void (*key)(film_app_t *app, film_key_t key, bool pressed);
} screen_ops_t;

/* ---------------------------------------------------------------- 持久化设置 */
#define SETTINGS_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t film;
    uint8_t instant;
    uint8_t date_stamp;
    uint16_t roll;
    uint8_t frame;          /*!< 本卷已拍张数 0..36 */
    uint8_t reserved;
} film_settings_t;

/* ---------------------------------------------------------------- 缩略图缓存 */
#define THUMB_MAX_W 160
#define THUMB_MAX_H 160
#define THUMB_CACHE_SLOTS 24
#define THUMB_LOADS_PER_FRAME 2

typedef struct {
    uint32_t id;            /*!< 0 表示空槽 */
    uint32_t used;          /*!< 最近使用的帧号（LRU） */
    uint16_t w, h;
    bool failed;
    uint16_t *pixels;
} thumb_slot_t;

/* ---------------------------------------------------------------- 相机页 */
#define CAM_STACK_PRINT_PX 58
typedef enum {
    CAM_OVL_NONE = 0,
    CAM_OVL_DIAL,           /*!< M6 大拨盘选胶卷 */
    CAM_OVL_POPOVER,        /*!< 长按胶卷窗：INSTANT / DATE */
    CAM_OVL_ROLLEND,        /*!< 一卷拍完 */
    CAM_OVL_DRAWER,         /*!< SX-70 换胶片盒 */
    CAM_OVL_DEVELOPING,     /*!< SX-70 显影 */
    CAM_OVL_PICKER,         /*!< 从顶部下拉的机身选择（M6 / SX-70） */
} cam_overlay_t;

typedef enum {
    SHOT_IDLE = 0,
    SHOT_FLASH,             /*!< 快门帘：取景黑一下 */
    SHOT_HOLD,              /*!< 定格刚拍的原片画面，等暗房送回成片（M6） */
    SHOT_REVEAL,            /*!< 原片淡出、冲洗后的成片淡入并停留片刻（M6） */
    SHOT_FLY,               /*!< 照片飞进计数窗 */
} shot_phase_t;

typedef struct {
    float ev;
    float ev_drag_start;
    uint32_t ev_scale_until;
    float film_pos;             /*!< 胶卷滚筒的显示位置（动画），整数即选中 */
    int film_drag_start;
    bool dragging_film;
    bool dragging_ev;

    bool shutter_down;
    bool window_down;
    bool counter_down;
    bool pack_down;
    uint8_t pressed_btn;        /*!< 弹出层里按下的按钮编号，0 表示没有 */
    bool dial_dragging;
    float dial_drag_pos;

    cam_overlay_t overlay;
    uint32_t overlay_t0;
    float plate_y;              /*!< M6 铝板上沿（360 收起 / 300 拨盘） */

    shot_phase_t shot;
    uint32_t shot_t0;
    gfx_rot_t shot_rot;         /*!< 按快门时的握持方向（成片按它转正） */
    uint16_t *frozen;           /*!< 快门瞬间的取景定格（480×360） */
    uint16_t frozen_w, frozen_h;
    bool shot_pending_rollend;

    /* 刚拍的成片：SX-70 显影动画与 M6 拍后回看共用（两种机身不会同时拍摄） */
    uint32_t dev_id;
    uint16_t *dev_print;        /*!< SX-70：宝丽来相纸屏幕图；M6：转回取景方向的成片（480×360） */
    uint16_t dev_w, dev_h;
    bool dev_ready;
    uint16_t dev_progress;      /*!< 全尺寸冲洗的真实进度（千分比）；FILM_PROGRESS_DONE 时取景已恢复 */
    uint32_t dev_emerge_t0;
    uint32_t dev_done_at;

    /* 机身选择面板：pull 为展开程度 0..1（拖动时跟手，松手后动画到 0 或 1） */
    float picker_pull;
    bool picker_open;           /*!< 松手后的目标：展开还是收起 */
    bool picker_dragging;
    float picker_drag_from;     /*!< 开始拖动时的 pull */
    int8_t picker_pressed;      /*!< 按下的卡片：-1 无、0 M6、1 SX-70 */

    /* 皮革合拢换机身 */
    bool skin_swapping;
    uint32_t skin_t0;
    bool skin_target_instant;
    bool skin_swapped;

    /* SX-70 取景下方的相纸堆（两张最新照片合成的小相纸，各 CAM_STACK_PRINT_PX 见方） */
    uint32_t stack_ids[2];
    uint16_t *stack_px[2];
    bool stack_valid[2];
} cam_state_t;

/* ---------------------------------------------------------------- 相册 / 大图 / 重洗 / 分享 */
typedef struct {
    float scroll;
    float velocity;
    bool dragging;
    float drag_start_scroll;
    bool selecting;
    uint32_t selected[SELECT_MAX];
    size_t n_selected;
    bool confirm_delete;        /*!< 多选删除的确认弹窗开着 */
} album_state_t;

typedef struct {
    size_t index;               /*!< 当前照片在相册中的序号（0 最新） */
    uint32_t loaded_id;
    uint16_t *pixels;           /*!< 屏幕图缓冲（最大 480×360） */
    uint16_t w, h;
    bool load_failed;
    int16_t drag_offset;
    float slide;                /*!< 切换照片时的滑入偏移 */
    bool confirm_delete;
} detail_state_t;

typedef struct {
    uint32_t source_id;
    float film_pos;
    int target_film;
    bool instant;
    bool holding_original;
    uint16_t *original;         /*!< 未冲洗的原片预览 */
    uint16_t orig_w, orig_h;
    bool orig_ready;
    uint16_t *result;           /*!< 当前胶卷的冲洗预览 */
    uint16_t res_w, res_h;
    int res_film;
    bool res_instant;
    bool request_in_flight;
    bool want_raw;
    uint32_t settle_at;         /*!< 滚筒停稳后再请求预览 */
    bool developing;            /*!< 已提交正式冲洗 */
    uint32_t new_id;
    bool dragging;
    int drag_start_film;
    float drag_start_pos;
    bool hold_down;
} redev_state_t;

#define SHARE_STACK_MAX 5
#define SHARE_CARD_W 200
#define SHARE_CARD_H 236
#define SHARE_MEDIA_W 66            /*!< 右侧小相纸（多张时叠放） */
#define SHARE_MEDIA_H 54
#define SHARE_QR_VERSION_MAX 12     /*!< 网址与 Wi-Fi 串都远小于 v12 容量 */
#define SHARE_QR_BUF_LEN ((((SHARE_QR_VERSION_MAX) * 4 + 17) * ((SHARE_QR_VERSION_MAX) * 4 + 17) + 7) / 8 + 1)

typedef struct {
    uint32_t ids[SELECT_MAX];
    size_t count;
    uint32_t total_bytes;
    film_share_status_t status;
    char qr_text[FILM_SHARE_TEXT_LEN * 2 + 32];
    char qr_next[FILM_SHARE_TEXT_LEN * 2 + 32];  /*!< 比较用的临时串 */
    uint8_t qr[SHARE_QR_BUF_LEN];       /*!< 二维码编码结果与临时区（放在实例里，不占任务栈） */
    uint8_t qr_tmp[SHARE_QR_BUF_LEN];
    uint16_t *card;             /*!< 二维码卡片离屏缓冲 */
    bool card_valid;
    uint16_t *media;            /*!< SHARE_STACK_MAX 张小相纸的离屏缓冲 */
    bool media_valid[SHARE_STACK_MAX];
    screen_id_t return_to;
    bool started;
    bool button_hotspot_down;
} share_state_t;

/* ---------------------------------------------------------------- 应用 */
struct film_app_t {
    const film_port_t *port;
    film_library_handle_t library;
    film_settings_t settings;
    uint32_t rng;
    uint32_t now;
    uint32_t frame_no;
    bool dirty;

    screen_id_t screen;
    const screen_ops_t *ops;

    /* 触摸手势识别 */
    struct {
        bool down;
        bool dragging;
        bool long_fired;
        bool horizontal;
        int16_t x0, y0, x, y;
        uint32_t t0;
        uint32_t last_tap_t;
        int16_t last_tap_x, last_tap_y;
        int16_t px, py;
        uint32_t pt;
        float vx, vy;
    } touch;

    /* 姿态：横竖、水平仪、摇一摇 */
    gfx_rot_t rot;
    gfx_rot_t rot_candidate;
    uint32_t rot_candidate_since;
    float gravity[3];
    bool gravity_valid;
    float tilt_deg;
    uint32_t shake_peaks[4];
    uint32_t shake_cooldown_until;

    /* 取景 */
    const film_frame_t *frame;
    uint32_t frame_seq;
    bool camera_ok;
    bool storage_ok;
    bool preview_paused;        /*!< 已请求平台暂停取景转换（机身选择面板打开期间画面定格） */

    /* 电量 */
    film_battery_t battery;
    bool battery_valid;
    bool battery_warned;        /*!< 已提示过电量低；回升或充电后清除 */
    uint32_t battery_polled_at;

    /* 提示条 */
    char toast[96];
    uint32_t toast_until;

    thumb_slot_t thumbs[THUMB_CACHE_SLOTS];
    uint16_t *thumb_pool;
    int thumb_loads;            /*!< 本帧已从文件加载的缩略图数（限流） */

    cam_state_t cam;
    album_state_t album;
    detail_state_t detail;
    redev_state_t redev;
    share_state_t share;
};

/* ---------------------------------------------------------------- 页面接口表 */
extern const screen_ops_t g_screen_camera;
extern const screen_ops_t g_screen_album;
extern const screen_ops_t g_screen_detail;
extern const screen_ops_t g_screen_redevelop;
extern const screen_ops_t g_screen_share;

/* ---------------------------------------------------------------- app.c 提供的公共服务 */
void app_go(film_app_t *app, screen_id_t screen);
void app_toast(film_app_t *app, const char *text);
void app_toast_error(film_app_t *app, esp_err_t err);
void app_feedback(film_app_t *app, film_feedback_t kind);
void app_save_settings(film_app_t *app);
uint32_t app_random(film_app_t *app);
/** 当前取景参数（胶卷、曝光、机身）变化后通知平台 */
void app_update_preview(film_app_t *app);
/** 取缩略图（未加载时排队加载，返回 NULL） */
const thumb_slot_t *app_thumb(film_app_t *app, uint32_t id);
/** 照片总字节数（分享页显示） */
uint32_t app_photos_bytes(film_app_t *app, const uint32_t *ids, size_t count);
/** 打开分享页 */
void app_share(film_app_t *app, const uint32_t *ids, size_t count, screen_id_t return_to);
/** 打开大图（序号 0 为最新） */
void app_open_detail(film_app_t *app, size_t index);
void app_open_redevelop(film_app_t *app, uint32_t source_id);
/** 暗房各页共用的按键行为：按下快门键回到取景（不拍照） */
void app_key_back_to_camera(film_app_t *app, film_key_t key, bool pressed);

/* ---------------------------------------------------------------- 胶卷文字 */
typedef struct {
    const char *name;       /*!< "GOLD 200" */
    const char *word;       /*!< "GOLD" */
    const char *number;     /*!< "200" */
    const char *iso;        /*!< "ISO 200" */
    const char *tagline;
    uint32_t label_color;
} film_info_t;

const film_info_t *film_info(int film);
int film_wrap(int film);

/* ---------------------------------------------------------------- 绘制辅助（ui_draw.c） */

/** 一个"用户方向"坐标系：屏幕上的矩形 screen，内容按 rot 转正后宽 w、高 h */
typedef struct {
    gfx_rect_t screen;
    gfx_rot_t rot;
    int w, h;
} ui_frame_t;

ui_frame_t ui_frame(gfx_rect_t screen, gfx_rot_t rot);
/** 用户坐标点 → 屏幕坐标点 */
void ui_point(const ui_frame_t *f, int u, int v, int *x, int *y);
/** 用户坐标矩形 → 屏幕矩形 */
gfx_rect_t ui_rect(const ui_frame_t *f, int u, int v, int w, int h);
/** 用户方向的位移向量 → 屏幕位移 */
void ui_rot_vec(gfx_rot_t rot, int du, int dv, int *dx, int *dy);
/** 屏幕位移 → 用户方向位移 */
void ui_unrot_vec(gfx_rot_t rot, int dx, int dy, int *du, int *dv);

/** 文字样式：size_px 只用于把 em 字距换算成像素 */
gfx_text_style_t ui_style(const gfx_font_t *font, float size_px, float tracking_em, uint32_t color, uint8_t alpha,
                          gfx_align_t align, gfx_rot_t rot);
/** 中文样式（Noto + Jost 西文回退） */
gfx_text_style_t ui_cjk(const gfx_font_t *font, uint32_t color, uint8_t alpha, gfx_align_t align, gfx_rot_t rot);

/** 铝板刻字（9px，.26em）；light 为皮革上的浅色刻字 */
void ui_engrave(gfx_canvas_t *c, int x, int y, const char *text, bool light, gfx_rot_t rot);
/** 带光晕的琥珀/橙色文字 */
void ui_glow_text(gfx_canvas_t *c, const gfx_font_t *core, const gfx_font_t *glow, float size, float tracking,
                  uint32_t color, uint8_t glow_alpha, gfx_align_t align, gfx_rot_t rot, int x, int y,
                  const char *text);

void ui_button_primary(gfx_canvas_t *c, gfx_rect_t r, const char *label, const gfx_image_t *icon, bool pressed,
                       bool enabled);
void ui_button_line(gfx_canvas_t *c, gfx_rect_t r, const char *label, const gfx_image_t *icon, bool pressed,
                    uint32_t ink, uint8_t line_alpha);
void ui_button_dark(gfx_canvas_t *c, gfx_rect_t r, const char *label, bool pressed, bool enabled);
void ui_icon_button(gfx_canvas_t *c, int x, int y, const gfx_image_t *icon, bool pressed);
void ui_check(gfx_canvas_t *c, int x, int y, bool on);
void ui_lever(gfx_canvas_t *c, int cx, int cy, bool on);
/** 琥珀色指针三角（尖朝 rot 方向的"下"） */
void ui_index_mark(gfx_canvas_t *c, int cx, int top, gfx_rot_t rot);
/** 带阴影的精灵图，(x, y) 为元素框左上角 */
void ui_sprite(gfx_canvas_t *c, const gfx_image_t *img, int ox, int oy, int x, int y, uint8_t opacity);
void ui_toast(gfx_canvas_t *c, const ui_frame_t *f, const char *text, uint8_t alpha);
/** 屏幕顶部的半透明渐变（大图、重洗页） */
void ui_top_shade(gfx_canvas_t *c, int height, uint8_t alpha);
/** 暗房页眉：返回键、标题、副标题、右侧文字 */
void ui_header(gfx_canvas_t *c, const char *title, const char *sub, const char *right, bool back_pressed);
/** 相纸（纸纹理 + 投影） */
void ui_paper(gfx_canvas_t *c, gfx_rect_t r, int shadow_blur, uint8_t shadow_alpha);
/** 删除确认弹窗（大图页与相册多选共用）：遮罩 + 卡片 + 问句 + CANCEL / DELETE */
void ui_delete_confirm(gfx_canvas_t *c, const char *question);
/** 删除确认弹窗里是否点中了 DELETE（点其他任何地方都算取消） */
bool ui_delete_confirm_hit(int x, int y);
/** 点是否在矩形内（可加边距） */
bool ui_hit(gfx_rect_t r, int x, int y, int slop);
/** 是否点在页眉返回键的点击区 */
bool ui_back_hit(int x, int y);
/** 圆角为 radius 的矩形是否整个落在屏幕圆角安全区内 */
bool ui_in_safe_area(gfx_rect_t r, int radius);
/** 仅模拟器：矩形越出圆角安全区时打一行 "ui-safe-area" 警告；设备上为空操作 */
void ui_safe_check(gfx_rect_t r, int radius, const char *what);

/** 格式化曝光值，如 "+0.7"、"-1.3"、"0" */
void ui_format_ev(float ev, char *buf, size_t len);
/** 格式化日期戳 "'26 9 30" */
void ui_format_stamp(int64_t time, char *buf, size_t len);

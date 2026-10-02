/*
 * film_gfx：在 RGB565 画布上绘制界面的最小软件光栅库（与平台无关，设备和模拟器共用）。
 *
 * 约定：
 *   - 画布像素为本机字节序 RGB565（GSP Canvas 的原生格式）；
 *   - 颜色参数用 0xRRGGBB，透明度单独给 0..255；
 *   - 图片为 RGB565 + 可选 A8 透明度（直通 alpha，非预乘）；
 *   - 文字来自离线烘焙的 A8 字形图集，可按 0/90/180/270 度旋转绘制（横竖拍时内容转正）。
 * 所有函数只在调用线程上访问画布，不持有全局可变状态。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 轴对齐矩形（左上角 + 宽高） */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} gfx_rect_t;

/** 绘制目标；clip 之外的像素不会被写入 */
typedef struct {
    uint16_t *pixels;
    int16_t width;
    int16_t height;
    int32_t stride;         /*!< 每行像素数（不是字节数） */
    gfx_rect_t clip;
} gfx_canvas_t;

/** RGB565 图片，alpha 为 NULL 表示不透明 */
typedef struct {
    const uint16_t *pixels;
    const uint8_t *alpha;
    uint16_t width;
    uint16_t height;
} gfx_image_t;

/** 一个字形：A8 位图在图集中的偏移与相对笔位置的摆放 */
typedef struct {
    uint32_t codepoint;
    uint32_t offset;
    uint8_t width;
    uint8_t height;
    int8_t left;            /*!< 位图左边相对笔位置 */
    int8_t top;             /*!< 位图上边相对基线（向上为负） */
    uint16_t advance_q4;    /*!< 前进量，1/16 像素 */
} gfx_glyph_t;

/** 字形图集（glyphs 按码点升序） */
typedef struct {
    const gfx_glyph_t *glyphs;
    const uint8_t *bitmap;
    uint16_t count;
    int8_t ascent;          /*!< 大写字母高度（像素），用于垂直居中 */
    int8_t descent;
} gfx_font_t;

/** 文字绘制方向：内容整体绕锚点顺时针旋转 */
typedef enum {
    GFX_ROT_0 = 0,
    GFX_ROT_90,
    GFX_ROT_180,
    GFX_ROT_270,
} gfx_rot_t;

typedef enum {
    GFX_ALIGN_LEFT = 0,
    GFX_ALIGN_CENTER,
    GFX_ALIGN_RIGHT,
} gfx_align_t;

/** 一段文字的样式；tracking_q4 为字间距（1/16 像素），对应 CSS letter-spacing */
typedef struct {
    const gfx_font_t *font;
    const gfx_font_t *fallback;  /*!< 主字体缺字时使用（中英混排），可为 NULL */
    uint32_t color;
    uint8_t alpha;
    int16_t tracking_q4;
    gfx_align_t align;
    gfx_rot_t rot;
} gfx_text_style_t;

static inline uint16_t gfx_rgb565(uint32_t rgb)
{
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

static inline gfx_rect_t gfx_rect(int x, int y, int w, int h)
{
    return (gfx_rect_t) { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
}

/* ---------- 画布 ---------- */

void gfx_canvas_init(gfx_canvas_t *canvas, uint16_t *pixels, int width, int height, int stride);
/** 把裁剪区设为 rect 与当前裁剪区的交集，返回之前的裁剪区以便恢复 */
gfx_rect_t gfx_clip_push(gfx_canvas_t *canvas, gfx_rect_t rect);
void gfx_clip_pop(gfx_canvas_t *canvas, gfx_rect_t previous);

/* ---------- 填充 ---------- */

void gfx_fill(gfx_canvas_t *c, gfx_rect_t r, uint32_t color, uint8_t alpha);
/** 圆角矩形（抗锯齿圆角） */
void gfx_fill_round(gfx_canvas_t *c, gfx_rect_t r, int radius, uint32_t color, uint8_t alpha);
/** 圆角矩形描边，width 像素宽，向内描 */
void gfx_stroke_round(gfx_canvas_t *c, gfx_rect_t r, int radius, int width, uint32_t color, uint8_t alpha);
/** 纵向线性渐变（可带圆角） */
void gfx_gradient_v(gfx_canvas_t *c, gfx_rect_t r, int radius, uint32_t top, uint8_t top_alpha,
                    uint32_t bottom, uint8_t bottom_alpha);
/** 横向线性渐变 */
void gfx_gradient_h(gfx_canvas_t *c, gfx_rect_t r, uint32_t left, uint8_t left_alpha,
                    uint32_t right, uint8_t right_alpha);
/** 实心圆（cx, cy, radius 均为 1/16 像素），抗锯齿边缘 */
void gfx_circle_q4(gfx_canvas_t *c, int cx_q4, int cy_q4, int radius_q4, uint32_t color, uint8_t alpha);
/** 圆环：外半径 radius，宽 width（1/16 像素） */
void gfx_ring_q4(gfx_canvas_t *c, int cx_q4, int cy_q4, int radius_q4, int width_q4, uint32_t color, uint8_t alpha);
/** 柔和光晕：中心 alpha 最大、到 radius 处衰减为 0 */
void gfx_glow(gfx_canvas_t *c, int cx, int cy, int radius, uint32_t color, uint8_t alpha);
/** 实心三角形（顶点为 1/16 像素），边缘 4×4 超采样抗锯齿 */
void gfx_triangle_q4(gfx_canvas_t *c, int x0, int y0, int x1, int y1, int x2, int y2, uint32_t color,
                     uint8_t alpha);
/**
 * 实心凸四边形（顶点为 1/16 像素，xy 交替共 8 个数，顺时针或逆时针均可）。
 * 逐行扫描：内部整段直接混色，只有边缘像素按 4 条子扫描线求覆盖率，大面积填充比两个三角形快一个数量级。
 */
void gfx_quad_q4(gfx_canvas_t *c, const int pts_q4[8], uint32_t color, uint8_t alpha);
/** 圆角矩形的柔和投影：r 为投影本体，向外 blur 像素线性衰减 */
void gfx_shadow(gfx_canvas_t *c, gfx_rect_t r, int radius, int blur, uint32_t color, uint8_t alpha);
/** 区域亮度乘以 factor_q8/256（变暗用） */
void gfx_dim(gfx_canvas_t *c, gfx_rect_t r, int factor_q8);

/* ---------- 图片 ---------- */

/** 原尺寸贴图（左上角在 x, y），opacity 0..255 */
void gfx_blit(gfx_canvas_t *c, const gfx_image_t *img, int x, int y, uint8_t opacity);
/** 贴图的一部分 */
void gfx_blit_part(gfx_canvas_t *c, const gfx_image_t *img, gfx_rect_t src, int x, int y, uint8_t opacity);
/** 不透明 RGB565 缓冲直接拷贝（取景帧、照片） */
void gfx_copy(gfx_canvas_t *c, const uint16_t *pixels, int width, int height, int stride, int x, int y);
/** 缩放贴图（双线性），把 src 区域映射到 dst */
void gfx_blit_scaled(gfx_canvas_t *c, const uint16_t *pixels, int width, int height, int stride,
                     gfx_rect_t src, gfx_rect_t dst, uint8_t opacity);
/** 按 cover 方式缩放填满 dst（居中裁切） */
void gfx_blit_cover(gfx_canvas_t *c, const uint16_t *pixels, int width, int height, int stride, gfx_rect_t dst,
                    uint8_t opacity);
/**
 * 任意角度旋转贴图：src 的中心落在 (cx_q4, cy_q4)，旋转 angle_deg（顺时针为正），缩放 scale，双线性采样。
 * alpha 为 NULL 表示源不透明。用于桌面上微微倾斜的相纸。
 */
void gfx_blit_rotated(gfx_canvas_t *c, const uint16_t *pixels, const uint8_t *alpha, int width, int height,
                      int stride, int cx_q4, int cy_q4, float angle_deg, float scale, uint8_t opacity);
/** 把纹理平铺到矩形内（纹理坐标从 origin 开始），可带圆角 */
void gfx_tile(gfx_canvas_t *c, const gfx_image_t *tex, gfx_rect_t r, int origin_x, int origin_y, int radius);
/** A8 遮罩着色，可按 90° 倍数旋转（遮罩左上角放在旋转后矩形的左上角 x, y） */
void gfx_mask(gfx_canvas_t *c, const uint8_t *mask, int width, int height, int stride, int x, int y,
              gfx_rot_t rot, uint32_t color, uint8_t alpha);
/** 以 (cx, cy) 为中心按 90° 倍数旋转绘制 A8 图标 */
void gfx_icon(gfx_canvas_t *c, const gfx_image_t *icon, int cx, int cy, gfx_rot_t rot, uint32_t color,
              uint8_t alpha);

/* ---------- 文字 ---------- */

/** 文字行宽（1/16 像素），含字间距但不含末尾一个 */
int gfx_text_width_q4(const gfx_text_style_t *style, const char *utf8);
/**
 * 绘制一行 UTF-8 文字。锚点 (x, y) 为文字在"用户方向"下的水平对齐点与垂直中心
 * （大写字母高度的中点），然后整体绕锚点旋转 style->rot。
 */
void gfx_text(gfx_canvas_t *c, const gfx_text_style_t *style, int x, int y, const char *utf8);
/**
 * 刻字效果：先在"用户下方"1 像素画一层高光，再画字本身（对应样稿的 text-shadow）。
 * highlight_alpha 为 0 时不画高光。
 */
void gfx_text_engraved(gfx_canvas_t *c, const gfx_text_style_t *style, int x, int y, const char *utf8,
                       uint32_t highlight, uint8_t highlight_alpha);
/** 按字节读取一个 UTF-8 码点，返回下一个字符位置；遇到非法序列按单字节处理 */
const char *gfx_utf8_next(const char *s, uint32_t *ret_codepoint);
const gfx_glyph_t *gfx_font_find(const gfx_font_t *font, uint32_t codepoint);

#ifdef __cplusplus
}
#endif

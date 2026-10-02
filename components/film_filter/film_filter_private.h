/*
 * film_filter 组件内部共享的定义：胶卷预设、实例结构、每次冲洗的上下文和各遍处理函数。
 * 只允许组件自己的 .c 文件包含。
 *
 * 并行模型：一次冲洗按"遍"进行，每一遍把图像按行切成上下两段，
 * 调用者处理上段、工作任务（另一个核）处理下段，两段都完成后才进入下一遍。
 * 每一遍内两段只读共享的查找表，写入互不重叠的像素行 / 网格行 / 格子行。
 */
#pragma once

#include "film_filter.h"
#include "film_worker.h"

#define FILM_CURVE_MAX_POINTS   5
#define FILM_CHANNELS           3

#define Q8_ONE                  256
#define Q8_MAX                  (255 * Q8_ONE)

/* 查表下标的位宽：半径平方映射到 0..FILM_RADIAL_LUT_SIZE-1 */
#define FILM_RADIAL_LUT_BITS    10
#define FILM_RADIAL_LUT_SIZE    ((1 << FILM_RADIAL_LUT_BITS) + 1)
/* 列/行表里半径平方的定点位数（1.0 == 1 << FILM_RADIAL_Q） */
#define FILM_RADIAL_Q           14
/*
 * 暗角：列表 + 行表 = 归一化半径平方 r²（Q14，范围 0..2），右移后作为 vignette_q15 的下标，
 * 每格对应 r² 的 2/1024。漏光：椭圆距离平方 d²（Q14，范围 0..1），每格对应 d² 的 1/1024。
 */
#define FILM_VIGNETTE_INDEX_SHIFT   (FILM_RADIAL_Q + 1 - FILM_RADIAL_LUT_BITS)
#define FILM_LEAK_INDEX_SHIFT       (FILM_RADIAL_Q - FILM_RADIAL_LUT_BITS)

/* 高光红晕在低分辨率网格上模糊：长边缩到不超过 GRID_MAX 格，每格最多 MAX_FACTOR 像素见方 */
#define FILM_HALATION_GRID_MAX      160
#define FILM_HALATION_MAX_FACTOR    16
/* 网格里存"高光遮罩均值 × 16"，保留 4 位小数精度 */
#define FILM_HALATION_MASK_SCALE    16

/* 颗粒噪声表：长度为 2 的幂，每行从随机偏移处开始连续取值；表尾再复制 max_width 项免去取模 */
#define FILM_GRAIN_TABLE_BITS   12
#define FILM_GRAIN_TABLE_SIZE   (1u << FILM_GRAIN_TABLE_BITS)

/* 像素风：长边固定 96 格（原型为 480 / 5） */
#define FILM_PIXEL_COLUMNS      96
#define FILM_PIXEL_PALETTE_SIZE 32

/* 行数少于这个值时不值得分给工作任务 */
#define FILM_PARALLEL_MIN_ROWS  16

typedef struct {
    uint8_t x;
    uint8_t y;
} film_curve_point_t;

typedef struct {
    uint8_t count;
    film_curve_point_t points[FILM_CURVE_MAX_POINTS];
} film_curve_t;

/** 一卷胶卷的全部调色参数，与原型 films.py 的 Film 字段一一对应 */
typedef struct {
    const char *key;
    film_curve_t curve[FILM_CHANNELS];  /*!< 黑白胶卷只用第 0 条 */
    float saturation;
    int8_t shadow_tint[FILM_CHANNELS];
    int8_t highlight_tint[FILM_CHANNELS];
    bool mono;
    float halation;
    float vignette;
    float grain;
    bool pixel;
} film_preset_t;

const film_preset_t *film_preset_get(film_id_t film);

/** 高光红晕网格的尺寸 */
typedef struct {
    uint16_t factor;    /*!< 每格边长（像素） */
    uint16_t width;
    uint16_t height;
} film_halation_grid_t;

typedef struct film_filter_t film_filter_t;

struct film_filter_t {
    uint16_t max_width;
    uint16_t max_height;
    film_worker_t *worker;                      /*!< 并行模式下的工作任务，否则为 NULL */

    /* 查找表缓存键：胶卷、曝光或尺寸不变时直接复用上一帧的表（取景时每帧都一样） */
    int16_t cached_film;                        /*!< -1 表示尚未建表 */
    int16_t cached_ev_centi;                    /*!< 曝光补偿 × 100 */
    uint16_t cached_width;
    uint16_t cached_height;
    bool leak_cached;                           /*!< 漏光行列表是否对应 cached_leak_seed 和当前尺寸 */
    uint32_t cached_leak_seed;
    /** halation_mask 里是上一帧（同胶卷同尺寸）已模糊好的红晕，可供快速红晕直接使用 */
    bool halation_valid;

    /* 按胶卷、曝光和尺寸建立的查找表；曝光补偿已折进曲线 */
    uint8_t exposure_lut[256];                 /*!< 输入色阶 → 补偿后的色阶（像素风用） */
    uint8_t curve_lut[FILM_CHANNELS][256];
    uint8_t curve_scratch[256];                 /*!< 建表时暂存未合成曝光的曲线 */
    int16_t tint_q8[FILM_CHANNELS][256];      /*!< 按亮度索引的色调偏移（Q8） */
    uint16_t grain_amp_q12[256];               /*!< 按亮度索引的颗粒幅度（Q12） */
    uint16_t vignette_q15[FILM_RADIAL_LUT_SIZE];
    uint16_t leak_q12[FILM_RADIAL_LUT_SIZE];

    /* 创建时生成、与种子无关的颗粒噪声，长度 FILM_GRAIN_TABLE_SIZE + max_width */
    int16_t *grain_noise;

    /* 按图像宽/高重建的行列表，长度为 max_width / max_height */
    uint16_t *vignette_col;
    uint16_t *vignette_row;
    uint16_t *leak_col;
    uint16_t *leak_row;

    /* 高光红晕的低分辨率网格及其双线性上采样表。
     * halation_next 只在快速红晕时使用：本帧累加新遮罩，同时从 halation_mask 读上一帧的结果 */
    uint32_t *halation_mask;
    uint32_t *halation_next;
    uint32_t *halation_line;                   /*!< 模糊暂存：网格一维长度 + 两端各 line_pad 格 */
    uint16_t *halation_x0;
    uint8_t *halation_wx;
    uint16_t halation_grid_capacity;           /*!< 网格一维最大长度 */
    uint16_t halation_line_pad;

    /* 像素风的格子颜色（FILM_PIXEL_COLUMNS² 个 RGB）和"列 → 格子列"表（max_width 项） */
    uint8_t *pixel_cells;
    uint8_t *pixel_col_cell;
};

/** 一次冲洗在各遍之间共享的只读参数（查找表在句柄里，冲洗前建好） */
typedef struct {
    film_filter_handle_t filter;
    const film_image_t *image;
    uint32_t seed;
    int32_t sat_q8;
    bool mono;
    bool halation;
    bool leak;
    bool grain;
    bool bgr;                                  /*!< 内存通道顺序为 B、G、R */
    bool exposure;                             /*!< 曝光补偿非零（像素风需要查 exposure_lut） */
    film_halation_grid_t grid;
    uint32_t *halation_accum;                  /*!< 调色时累加高光遮罩的网格 */
    int32_t halation_k_q16[FILM_CHANNELS];     /*!< 遮罩值 → Q8 加量的系数 */
    uint16_t pixel_cols;                       /*!< 像素风格子列数 / 行数 */
    uint16_t pixel_rows;
} film_develop_ctx_t;

static inline int32_t film_clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* 整数哈希（lowbias32），用于颗粒和漏光位置，保证同一种子结果可复现 */
static inline uint32_t film_hash32(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return v;
}

/* 彩色/黑白管线的各遍（film_color.c），处理 [y0, y1) 行 */
void film_color_fused_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1);     /*!< 无红晕：一遍完成 */
void film_color_tone_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1);      /*!< 红晕第一遍 */
void film_color_finish_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1);    /*!< 红晕第二遍 */
/** 快速红晕：调色 + 用上一帧遮罩收尾，同时累加本帧遮罩，一遍完成 */
void film_color_reuse_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1);

/* 高光红晕网格（film_halation.c） */
film_halation_grid_t film_halation_grid_for(uint16_t width, uint16_t height);
uint16_t film_halation_grid_capacity(uint16_t max_width, uint16_t max_height);
uint16_t film_halation_line_pad(uint16_t max_width);        /*!< 模糊暂存每端需要的余量格数 */
void film_halation_prepare(film_filter_handle_t handle, const film_halation_grid_t *grid, uint16_t width);
void film_halation_upsample_coord(uint16_t pos, const film_halation_grid_t *grid, uint16_t grid_len,
                                  uint16_t *ret_index, uint8_t *ret_weight);
/** 第一遍累加完遮罩后调用：换算成每格均值并模糊（单线程，网格很小） */
void film_halation_resolve(film_filter_handle_t handle, const film_halation_grid_t *grid, uint16_t width,
                           uint16_t height);

/* 像素风（film_pixel.c）：先按格子行求颜色，再按图像行放大 */
void film_pixel_prepare(film_develop_ctx_t *ctx);
void film_pixel_cell_rows(const film_develop_ctx_t *ctx, uint16_t row0, uint16_t row1);
void film_pixel_upsample_rows(const film_develop_ctx_t *ctx, uint16_t y0, uint16_t y1);

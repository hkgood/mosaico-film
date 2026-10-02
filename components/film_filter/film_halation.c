/*
 * 高光红晕的低分辨率网格：第一遍逐像素把高光遮罩累加进网格（film_color.c），
 * 这里负责网格尺寸、换算均值、模糊，以及全分辨率 ↔ 网格的双线性上采样坐标。
 * 网格最多 160 格见方，模糊在单线程里做就够了。
 */
#include <math.h>

#include "film_filter_private.h"

#define HALATION_SIGMA_DIV      60.0f       /*!< 模糊半径 = 图像宽 / 60 */
#define HALATION_BLUR_PASSES    3
#define HALATION_GRID_MAX_F     ((float)FILM_HALATION_GRID_MAX)

film_halation_grid_t film_halation_grid_for(uint16_t width, uint16_t height)
{
    uint32_t longest = width > height ? width : height;
    uint32_t factor = (longest + FILM_HALATION_GRID_MAX - 1) / FILM_HALATION_GRID_MAX;
    factor = (uint32_t)film_clamp_i32((int32_t)factor, 1, FILM_HALATION_MAX_FACTOR);
    return (film_halation_grid_t) {
        .factor = (uint16_t)factor,
        .width = (uint16_t)((width + factor - 1) / factor),
        .height = (uint16_t)((height + factor - 1) / factor),
    };
}

uint16_t film_halation_grid_capacity(uint16_t max_width, uint16_t max_height)
{
    uint32_t longest = max_width > max_height ? max_width : max_height;
    uint32_t capacity = longest < FILM_HALATION_GRID_MAX ? longest : FILM_HALATION_GRID_MAX;
    /* 超大图像每格已到上限 MAX_FACTOR 像素，网格只能跟着变长 */
    uint32_t oversized = (longest + FILM_HALATION_MAX_FACTOR - 1) / FILM_HALATION_MAX_FACTOR;
    return (uint16_t)(capacity > oversized ? capacity : oversized);
}

/*
 * 全分辨率坐标 pos 在网格上的双线性插值位置：格子 g 的中心位于 g × F + (F - 1) / 2。
 * 输出较小的那个格子下标和到下一格的权重（Q8）；落在网格边缘之外时权重为 0。
 */
void film_halation_upsample_coord(uint16_t pos, const film_halation_grid_t *grid, uint16_t grid_len,
                                  uint16_t *ret_index, uint8_t *ret_weight)
{
    float s = ((float)pos - (grid->factor - 1) / 2.0f) / grid->factor;
    if (s <= 0.0f) {
        *ret_index = 0;
        *ret_weight = 0;
    } else if (s >= grid_len - 1) {
        *ret_index = grid_len - 1;
        *ret_weight = 0;
    } else {
        uint16_t i0 = (uint16_t)s;
        *ret_index = i0;
        *ret_weight = (uint8_t)film_clamp_i32((int32_t)lroundf((s - i0) * Q8_ONE), 0, 255);
    }
}

void film_halation_prepare(film_filter_handle_t handle, const film_halation_grid_t *grid, uint16_t width)
{
    for (uint16_t x = 0; x < width; x++) {
        film_halation_upsample_coord(x, grid, grid->width, &handle->halation_x0[x], &handle->halation_wx[x]);
    }
}

/* 与 Pillow GaussianBlur 相同的"带小数半径的三次均值模糊"近似 */
static float box_radius_for_sigma(float sigma)
{
    float sigma2 = sigma * sigma / HALATION_BLUR_PASSES;
    float box_len = sqrtf(12.0f * sigma2 + 1.0f);
    float l = floorf((box_len - 1.0f) / 2.0f);
    float a = (2.0f * l + 1.0f) * (l * (l + 1.0f) - 3.0f * sigma2);
    a /= 6.0f * (sigma2 - (l + 1.0f) * (l + 1.0f));
    return l + a;
}

/*
 * 除以运行期常数 d 的精确整数除法（RISC-V 上一次 divu 要三十多个周期，这里换成一次 64 位乘法）。
 * 取 k = N + bits(d)、m = ceil(2^k / d)，则对所有 n < 2^N 有 (n × m) >> k == n / d：
 * 误差项 n × (m·d − 2^k) / (d · 2^k) < 2^N / 2^k = 2^-bits(d) < 1/d，不会越过整数边界。
 * 要求 N ≤ 30，使 m 放得进 32 位。
 */
typedef struct {
    uint32_t m;
    uint8_t shift;
} exact_div_t;

static uint8_t bit_length(uint32_t v)
{
    uint8_t bits = 0;
    while (v) {
        bits++;
        v >>= 1;
    }
    return bits;
}

static exact_div_t exact_div_for(uint32_t d, uint32_t n_max)
{
    uint8_t shift = (uint8_t)(bit_length(n_max) + bit_length(d));
    return (exact_div_t) {
        .m = (uint32_t)(((1ull << shift) + d - 1) / d), .shift = shift,
    };
}

static inline uint32_t exact_div(uint32_t n, exact_div_t div)
{
    return (uint32_t)(((uint64_t)n * div.m) >> div.shift);
}

/** 一维带小数半径均值模糊的参数 */
typedef struct {
    int whole;              /*!< 半径整数部分 */
    uint32_t frac_q8;       /*!< 半径小数部分：窗口两端外侧各一格按此权重计入 */
    uint32_t norm;          /*!< 权重总和 */
    exact_div_t div;        /*!< 除以 norm */
} box_kernel_t;

/*
 * 一维模糊一遍（边缘取最近值）。data 可跨步访问；line 为暂存，前后各留 whole + 1 格
 * 复制边缘值，相当于原来逐项 clamp 下标，但不再需要判断。窗口和用滑动累加。
 */
static void box_blur_line(uint32_t *data, size_t step, uint16_t count, uint32_t *line, const box_kernel_t *k)
{
    const int pad = k->whole + 1;
    uint32_t *t = line + pad;
    for (uint16_t i = 0; i < count; i++) {
        t[i] = data[i * step];
    }
    for (int i = 1; i <= pad; i++) {
        t[-i] = t[0];
        t[count - 1 + i] = t[count - 1];
    }

    uint32_t window = 0;
    for (int i = -k->whole; i <= k->whole; i++) {
        window += t[i];
    }
    for (int i = 0; i < count; i++) {
        uint32_t sum = window * Q8_ONE + k->frac_q8 * (t[i - k->whole - 1] + t[i + k->whole + 1]);
        data[i * step] = exact_div(sum + k->norm / 2, k->div);
        window += t[i + k->whole + 1] - t[i - k->whole];
    }
}

/* 把每格的遮罩累加和换成"均值 × MASK_SCALE"，边缘不满格的按实际像素数平均 */
static void normalize_mask(film_filter_handle_t handle, const film_halation_grid_t *grid,
                           uint16_t width, uint16_t height)
{
    uint32_t f = grid->factor;
    uint32_t full = f * f;
    /* 每格累加值不超过 255 × f² */
    exact_div_t full_div = exact_div_for(full, 255u * full * FILM_HALATION_MASK_SCALE + full);
    for (uint16_t gy = 0; gy < grid->height; gy++) {
        uint32_t ch = height - gy * f < f ? height - gy * f : f;
        uint32_t *row = &handle->halation_mask[(size_t)gy * grid->width];
        for (uint16_t gx = 0; gx < grid->width; gx++) {
            uint32_t cw = width - gx * f < f ? width - gx * f : f;
            uint32_t area = cw * ch;
            uint32_t n = row[gx] * FILM_HALATION_MASK_SCALE + area / 2;
            row[gx] = area == full ? exact_div(n, full_div) : n / area;
        }
    }
}

/*
 * 三次均值模糊。先把每行做完三遍再换下一行（与"每遍扫完所有行"结果相同，行之间互不影响）；
 * 列方向同理。列跨步访问时同一组 16 列落在同一批缓存行里，网格只有约 100 KB，缓存命中良好。
 */
static void blur_mask(film_filter_handle_t handle, const film_halation_grid_t *grid, uint16_t image_width)
{
    /* 原型在全分辨率上做 sigma = w/60 的模糊；网格下采样与双线性上采样本身
     * 各带来约 1/12、1/6 格² 的方差，从目标方差里扣掉 */
    float sigma = image_width / HALATION_SIGMA_DIV / grid->factor;
    float variance = sigma * sigma - 0.25f;
    if (variance <= 0.0f) {
        return;
    }
    float radius = box_radius_for_sigma(sqrtf(variance));
    box_kernel_t k = { .whole = (int)radius };
    k.frac_q8 = (uint32_t)lroundf((radius - (float)k.whole) * Q8_ONE);
    k.norm = (uint32_t)(2 * k.whole + 1) * Q8_ONE + 2 * k.frac_q8;
    /* 被除数 ≤ 最大遮罩值 × norm + norm / 2 */
    k.div = exact_div_for(k.norm, (255u * FILM_HALATION_MASK_SCALE + 1) * k.norm);
    if (k.whole + 1 > handle->halation_line_pad) {
        return;     /* 创建时按最大尺寸留足了余量，不会发生 */
    }

    uint32_t *mask = handle->halation_mask;
    for (uint16_t y = 0; y < grid->height; y++) {
        for (int pass = 0; pass < HALATION_BLUR_PASSES; pass++) {
            box_blur_line(mask + (size_t)y * grid->width, 1, grid->width, handle->halation_line, &k);
        }
    }
    for (uint16_t x = 0; x < grid->width; x++) {
        for (int pass = 0; pass < HALATION_BLUR_PASSES; pass++) {
            box_blur_line(mask + x, grid->width, grid->height, handle->halation_line, &k);
        }
    }
}

uint16_t film_halation_line_pad(uint16_t max_width)
{
    /* 网格上的 sigma = w / 60 / factor：factor 未封顶时不超过 160/60，封顶（16）后为 w/960。
     * 三次均值模糊的每次半径约为 sigma，这里留两倍余量 */
    float sigma = HALATION_GRID_MAX_F / HALATION_SIGMA_DIV;
    float oversized = (float)max_width / HALATION_SIGMA_DIV / FILM_HALATION_MAX_FACTOR;
    sigma = sigma > oversized ? sigma : oversized;
    return (uint16_t)(2.0f * sigma + 2.0f);
}

void film_halation_resolve(film_filter_handle_t handle, const film_halation_grid_t *grid, uint16_t width,
                           uint16_t height)
{
    normalize_mask(handle, grid, width, height);
    blur_mask(handle, grid, width);
}

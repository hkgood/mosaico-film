/*
 * 原片白平衡与亮度对齐，编码格式见 film_balance.h。
 */
#include "film_balance.h"

#include <math.h>
#include <string.h>

/* 抽样步长（行、列都隔 BALANCE_STEP 取一个） */
#define BALANCE_STEP 4
/* 任一通道达到这个值视为过曝，饱和像素的颜色不可信 */
#define BALANCE_CLIP 250
/* 最亮通道不超过这个值视为全黑，只剩噪声 */
#define BALANCE_FLOOR 6
/* 有效样本至少这么多，统计才算数 */
#define BALANCE_MIN_SAMPLES 64
/* 均值低于这个值不参与求比值，避免除以接近 0 的数 */
#define BALANCE_MIN_MEAN 1e-4f
/* 二分求增益的步数：4.75 档的区间收敛到 0.0003 档以内 */
#define BALANCE_SOLVE_STEPS 14

/* 提亮时线性光超过这个值开始压缩高光（约为 8 位值 180） */
#define BALANCE_KNEE 0.5f

/* 编码各字段 */
#define CHROMA_STEPS 16.0f      /* log2(R/G)、log2(B/G) 每档的格数 */
#define CHROMA_MIN (-32)
#define CHROMA_MAX 31
#define CHROMA_MASK 0x3Fu
#define LUMA_STEPS 4.0f         /* log2(G) 粗字段每档的格数 */
#define LUMA_MIN (-4)
#define LUMA_MAX 11
#define LUMA_MASK 0x0Fu
#define FINE_PER_LUMA 4         /* 粗字段一格再分几份（细分字段 0..3） */
#define FINE_STEPS (LUMA_STEPS * FINE_PER_LUMA)
#define FINE_MIN (LUMA_MIN * FINE_PER_LUMA)
#define FINE_MAX (LUMA_MAX * FINE_PER_LUMA + FINE_PER_LUMA - 1)
#define R_SHIFT 10
#define B_SHIFT 4

static float to_linear(int v)
{
    return (float)(v * v) * (1.0f / (255.0f * 255.0f));
}

static int clamp_int(long v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : (int)v);
}

/* 有符号字段解码：字段值大于 max 的部分是负数（按字段宽度回绕） */
static int signed_field(uint16_t code, int shift, unsigned mask, int max)
{
    const int v = (int)((code >> shift) & mask);
    return v > max ? v - (int)(mask + 1) : v;
}

/*
 * 线性光乘增益 g，并保持白点（x = 1 时 y = 1），过曝的白不会被染色：
 *   g ≥ 1：g·x ≤ 膝点时严格相乘；之上把 [膝点, g] 用 1 - (1 - u)^n 收进 [膝点, 1]，
 *          n 取得让膝点处斜率连续，高光是压缩而不是硬截断。
 *   g < 1：g·x 加一个只在接近白时起作用的高次项，把白拉回 1。
 */
static float apply_gain(float x, float g)
{
    if (g < 1.0f) {
        const float x2 = x * x;
        const float x4 = x2 * x2;
        return g * x + (1.0f - g) * x4 * x4;
    }
    const float t = g * x;
    if (t <= BALANCE_KNEE || g <= 1.0f) {
        return t;
    }
    const float u = (t - BALANCE_KNEE) / (g - BALANCE_KNEE);
    const float n = (g - BALANCE_KNEE) / (1.0f - BALANCE_KNEE);
    return BALANCE_KNEE + (1.0f - BALANCE_KNEE) * (1.0f - powf(1.0f - u, n));
}

void film_balance_measure(const uint8_t *pixels, size_t stride, int width, int height, film_tone_stats_t *ret_stats,
                          film_tone_hist_t *ret_hist)
{
    double sum[3] = { 0 };
    if (ret_hist) {
        memset(ret_hist, 0, sizeof(*ret_hist));
    }
    uint32_t samples = 0;
    for (int y = BALANCE_STEP / 2; y < height; y += BALANCE_STEP) {
        const uint8_t *p = pixels + (size_t)y * stride;
        for (int x = BALANCE_STEP / 2; x < width; x += BALANCE_STEP) {
            const uint8_t *px = p + (size_t)x * 3;
            const int hi = px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2]) : (px[1] > px[2] ? px[1] : px[2]);
            if (hi >= BALANCE_CLIP || hi <= BALANCE_FLOOR) {
                continue;
            }
            for (int c = 0; c < 3; ++c) {
                sum[c] += to_linear(px[c]);
                if (ret_hist) {
                    ++ret_hist->count[c][px[c]];
                }
            }
            ++samples;
        }
    }
    ret_stats->samples = samples;
    ret_stats->valid = samples >= BALANCE_MIN_SAMPLES;
    for (int c = 0; c < 3; ++c) {
        ret_stats->mean[c] = samples ? (float)(sum[c] / samples) : 0.0f;
    }
}

/* luma 以 1/16 档为单位（FINE_MIN..FINE_MAX），拆成粗字段 + 细分字段 */
static film_balance_t encode(int luma, int red, int blue)
{
    const int coarse = (luma - FINE_MIN) / FINE_PER_LUMA + LUMA_MIN;
    const int fine = luma - coarse * FINE_PER_LUMA;
    return ((film_balance_t)fine << FILM_BALANCE_FINE_SHIFT) | (((unsigned)red & CHROMA_MASK) << R_SHIFT) |
           (((unsigned)blue & CHROMA_MASK) << B_SHIFT) | ((unsigned)coarse & LUMA_MASK);
}

/* 亮度（档） */
static float luma_stops(film_balance_t code)
{
    const int coarse = signed_field((uint16_t)code, 0, LUMA_MASK, LUMA_MAX);
    const int fine = (int)((code >> FILM_BALANCE_FINE_SHIFT) & FILM_BALANCE_FINE_MASK);
    return (float)(coarse * FINE_PER_LUMA + fine) / FINE_STEPS;
}

/* 按直方图算校正后的线性均值（与 build_lut 用同一条曲线） */
static float mean_after(const uint32_t count[256], uint32_t samples, float stops)
{
    const float g = exp2f(stops);
    double sum = 0.0;
    for (int v = 0; v < 256; ++v) {
        if (count[v]) {
            sum += (double)count[v] * apply_gain(to_linear(v), g);
        }
    }
    return (float)(sum / samples);
}

/* 在 [lo, hi]（档）里二分出让校正后均值等于 target 的增益；均值随增益单调递增 */
static float solve_stops(const uint32_t count[256], uint32_t samples, float target, float lo, float hi)
{
    for (int i = 0; i < BALANCE_SOLVE_STEPS; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (mean_after(count, samples, mid) < target) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

/*
 * 精确求解：高光压缩会让校正后的均值低于"均值 × 增益"，直接用均值比会少补（实测约 10%）。
 * 先定亮度（G）的量化格（1/16 档），再在这一格之上求 R、B 的色度格。
 */
static film_balance_t match_hist(const film_tone_stats_t *from, const film_tone_hist_t *hist,
                                 const film_tone_stats_t *to)
{
    const uint32_t n = from->samples;
    const float g_stops = solve_stops(hist->count[1], n, to->mean[1], (float)FINE_MIN / FINE_STEPS,
                                      (float)FINE_MAX / FINE_STEPS);
    const int luma = clamp_int(lroundf(g_stops * FINE_STEPS), FINE_MIN, FINE_MAX);
    const float base = (float)luma / FINE_STEPS;
    int chroma[2];
    for (int k = 0; k < 2; ++k) {
        const int c = k == 0 ? 0 : 2;
        const float stops = solve_stops(hist->count[c], n, to->mean[c], base + (float)CHROMA_MIN / CHROMA_STEPS,
                                        base + (float)CHROMA_MAX / CHROMA_STEPS);
        chroma[k] = clamp_int(lroundf((stops - base) * CHROMA_STEPS), CHROMA_MIN, CHROMA_MAX);
    }
    return encode(luma, chroma[0], chroma[1]);
}

film_balance_t film_balance_match(const film_tone_stats_t *from, const film_tone_hist_t *from_hist,
                                  const film_tone_stats_t *to)
{
    if (!from || !to || !from->valid || !to->valid) {
        return FILM_BALANCE_NONE;
    }
    for (int c = 0; c < 3; ++c) {
        if (from->mean[c] < BALANCE_MIN_MEAN || to->mean[c] < BALANCE_MIN_MEAN) {
            return FILM_BALANCE_NONE;
        }
    }
    if (from_hist) {
        return match_hist(from, from_hist, to);
    }
    const float gain_r = to->mean[0] / from->mean[0];
    const float gain_g = to->mean[1] / from->mean[1];
    const float gain_b = to->mean[2] / from->mean[2];
    const int luma = clamp_int(lroundf(log2f(gain_g) * FINE_STEPS), FINE_MIN, FINE_MAX);
    const int red = clamp_int(lroundf(log2f(gain_r / gain_g) * CHROMA_STEPS), CHROMA_MIN, CHROMA_MAX);
    const int blue = clamp_int(lroundf(log2f(gain_b / gain_g) * CHROMA_STEPS), CHROMA_MIN, CHROMA_MAX);
    return encode(luma, red, blue);
}

void film_balance_gains(film_balance_t code, float ret_gain[3])
{
    const float luma = luma_stops(code);
    const float red = (float)signed_field((uint16_t)code, R_SHIFT, CHROMA_MASK, CHROMA_MAX) / CHROMA_STEPS;
    const float blue = (float)signed_field((uint16_t)code, B_SHIFT, CHROMA_MASK, CHROMA_MAX) / CHROMA_STEPS;
    ret_gain[0] = exp2f(luma + red);
    ret_gain[1] = exp2f(luma);
    ret_gain[2] = exp2f(luma + blue);
}

void film_balance_build_lut(film_balance_t code, uint8_t lut[3][256])
{
    float gain[3];
    film_balance_gains(code, gain);
    for (int c = 0; c < 3; ++c) {
        for (int v = 0; v < 256; ++v) {
            const float y = apply_gain(to_linear(v), gain[c]);
            lut[c][v] = (uint8_t)clamp_int(lroundf(sqrtf(y) * 255.0f), 0, 255);
        }
    }
}

void film_balance_apply(uint8_t *pixels, size_t stride, int width, int height, const uint8_t lut[3][256])
{
    for (int y = 0; y < height; ++y) {
        uint8_t *p = pixels + (size_t)y * stride;
        for (int x = 0; x < width; ++x, p += 3) {
            p[0] = lut[0][p[0]];
            p[1] = lut[1][p[1]];
            p[2] = lut[2][p[2]];
        }
    }
}

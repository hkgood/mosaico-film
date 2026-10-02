/*
 * 胶卷滤镜的入口：实例生命周期、查找表（带缓存）和"按遍分发"。
 *
 * 逐像素部分在 film_color.c / film_pixel.c，红晕网格在 film_halation.c。
 * 浮点只用于建表；取景时胶卷和尺寸每帧都一样，表只在变化时重建。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "film_filter_private.h"

/* 暗角：d = clip((r - INNER) / SPAN)，遮罩 = 1 - 强度 × d^EXPONENT */
#define VIGNETTE_INNER      0.45f
#define VIGNETTE_SPAN       0.95f
#define VIGNETTE_EXPONENT   1.6f

/* 高光红晕的颜色（每满格遮罩加多少色阶，再乘胶卷的 halation 强度） */
static const uint8_t s_halation_color[FILM_CHANNELS] = { 90, 18, 0 };

/* 漏光：从左或右边缘外侧射入的椭圆光斑 */
#define LEAK_STRENGTH       0.55f
#define LEAK_EXPONENT       1.8f
#define LEAK_RADIUS_X       0.55f       /*!< 椭圆半轴，相对图像宽 */
#define LEAK_RADIUS_Y       0.9f        /*!< 椭圆半轴，相对图像高 */
#define LEAK_CENTER_LEFT    (-0.05f)
#define LEAK_CENTER_RIGHT   1.05f
#define LEAK_CENTER_Y_MIN   0.2f
#define LEAK_CENTER_Y_RANGE 0.6f

/* 颗粒：幅度 = 强度 × 255 × (BASE + MID × l × (1 - l))，中间调颗粒最明显 */
#define GRAIN_BASE          0.35f
#define GRAIN_MID           1.3f
/* 4 个 0..255 均匀随机数之和的均值与标准差：sqrt(4 × (256² - 1) / 12) */
#define GRAIN_NOISE_MEAN    510
#define GRAIN_NOISE_STD     147.80f
/* 生成噪声表的哈希参数 */
#define GRAIN_TABLE_STEP    0x9E3779B1u
#define GRAIN_TABLE_SALT    0x632BE5ABu

#define TABLES_INVALID      (-1)
#define EV_CENTI_INVALID    INT16_MAX

static inline float clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* ---------- 生命周期 ---------- */

/*
 * 近似正态分布的颗粒噪声：4 个字节之和减去均值，范围约 ±3.45σ。
 * 表尾追加 max_width 项（与表头相同），每行从任意偏移开始都能连续取满一行。
 */
static void build_grain_noise(int16_t *noise, uint16_t max_width)
{
    for (uint32_t i = 0; i < FILM_GRAIN_TABLE_SIZE + max_width; i++) {
        uint32_t h = film_hash32((i & (FILM_GRAIN_TABLE_SIZE - 1)) * GRAIN_TABLE_STEP + GRAIN_TABLE_SALT);
        noise[i] = (int16_t)((int32_t)((h & 0xFF) + ((h >> 8) & 0xFF) + ((h >> 16) & 0xFF) + (h >> 24))
                             - GRAIN_NOISE_MEAN);
    }
}

/* 漏光强度只与椭圆距离有关，与胶卷、种子、尺寸都无关，创建时建一次 */
static void build_leak_lut(film_filter_handle_t handle)
{
    for (int i = 0; i < FILM_RADIAL_LUT_SIZE; i++) {
        float d = sqrtf((float)i / (float)(FILM_RADIAL_LUT_SIZE - 1));
        float a = powf(1.0f - d, LEAK_EXPONENT) * LEAK_STRENGTH;
        handle->leak_q12[i] = (uint16_t)lroundf(a * 4096.0f);
    }
}

esp_err_t film_filter_create(const film_filter_config_t *config, film_filter_handle_t *ret_handle)
{
    if (config == NULL || ret_handle == NULL || config->max_width == 0 || config->max_height == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    film_filter_handle_t handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    handle->max_width = config->max_width;
    handle->max_height = config->max_height;
    handle->cached_film = TABLES_INVALID;
    handle->cached_ev_centi = EV_CENTI_INVALID;
    handle->halation_grid_capacity = film_halation_grid_capacity(config->max_width, config->max_height);
    handle->halation_line_pad = film_halation_line_pad(config->max_width);

    size_t grid = handle->halation_grid_capacity;
    handle->grain_noise = calloc(FILM_GRAIN_TABLE_SIZE + config->max_width, sizeof(int16_t));
    handle->vignette_col = calloc(config->max_width, sizeof(uint16_t));
    handle->vignette_row = calloc(config->max_height, sizeof(uint16_t));
    handle->leak_col = calloc(config->max_width, sizeof(uint16_t));
    handle->leak_row = calloc(config->max_height, sizeof(uint16_t));
    handle->halation_mask = calloc(grid * grid, sizeof(uint32_t));
    handle->halation_next = calloc(grid * grid, sizeof(uint32_t));
    handle->halation_line = calloc(grid + 2u * handle->halation_line_pad, sizeof(uint32_t));
    handle->halation_x0 = calloc(config->max_width, sizeof(uint16_t));
    handle->halation_wx = calloc(config->max_width, sizeof(uint8_t));
    handle->pixel_cells = calloc(FILM_PIXEL_COLUMNS * FILM_PIXEL_COLUMNS * FILM_CHANNELS, 1);
    handle->pixel_col_cell = calloc(config->max_width, sizeof(uint8_t));

    if (!handle->grain_noise || !handle->vignette_col || !handle->vignette_row || !handle->leak_col ||
            !handle->leak_row || !handle->halation_mask || !handle->halation_next || !handle->halation_line || !handle->halation_x0 ||
            !handle->halation_wx || !handle->pixel_cells || !handle->pixel_col_cell) {
        film_filter_delete(handle);
        return ESP_ERR_NO_MEM;
    }
    if (config->parallel) {
        esp_err_t err = film_worker_create(&handle->worker);
        if (err != ESP_OK) {
            film_filter_delete(handle);
            return err;
        }
    }
    build_grain_noise(handle->grain_noise, config->max_width);
    build_leak_lut(handle);
    *ret_handle = handle;
    return ESP_OK;
}

esp_err_t film_filter_delete(film_filter_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    film_worker_delete(handle->worker);
    free(handle->grain_noise);
    free(handle->vignette_col);
    free(handle->vignette_row);
    free(handle->leak_col);
    free(handle->leak_row);
    free(handle->halation_mask);
    free(handle->halation_next);
    free(handle->halation_line);
    free(handle->halation_x0);
    free(handle->halation_wx);
    free(handle->pixel_cells);
    free(handle->pixel_col_cell);
    free(handle);
    return ESP_OK;
}

/* ---------- 建表 ---------- */

/* 分段线性插值控制点，等价于 np.interp；控制点按输入升序排列 */
static void build_curve_lut(const film_curve_t *curve, uint8_t *lut)
{
    const film_curve_point_t *p = curve->points;
    int seg = 0;
    for (int i = 0; i < 256; i++) {
        while (seg + 2 < curve->count && i > p[seg + 1].x) {
            seg++;
        }
        const film_curve_point_t *a = &p[seg];
        const film_curve_point_t *b = &p[seg + 1];
        float v;
        if (i <= a->x) {
            v = a->y;
        } else if (i >= b->x) {
            v = b->y;
        } else {
            v = a->y + (float)(b->y - a->y) * (float)(i - a->x) / (float)(b->x - a->x);
        }
        lut[i] = (uint8_t)film_clamp_i32((int32_t)lroundf(v), 0, 255);
    }
}

/*
 * 曝光补偿 g = 2^ev，作用在线性化之前的 0..1 色阶上：
 *   加曝光用 x·g / (1 + (g-1)·x)：暗部按 g 倍提亮，高光平滑压向 1，不会成片溢出；
 *   减曝光直接 x·g，高光一起变暗，与胶片欠曝的观感一致。
 */
static void build_exposure_lut(uint8_t *lut, int16_t ev_centi)
{
    float g = exp2f((float)ev_centi / 100.0f);
    for (int i = 0; i < 256; i++) {
        float x = (float)i / 255.0f;
        float y = g >= 1.0f ? x * g / (1.0f + (g - 1.0f) * x) : x * g;
        lut[i] = (uint8_t)film_clamp_i32((int32_t)lroundf(y * 255.0f), 0, 255);
    }
}

static void build_tone_tables(film_filter_handle_t handle, const film_preset_t *preset, int16_t ev_centi)
{
    int curves = preset->mono ? 1 : FILM_CHANNELS;
    build_exposure_lut(handle->exposure_lut, ev_centi);
    for (int c = 0; c < curves; c++) {
        build_curve_lut(&preset->curve[c], handle->curve_scratch);
        /* 曝光折进曲线：curve'(i) = curve(exposure(i))，逐像素仍然只查一次表 */
        for (int i = 0; i < 256; i++) {
            handle->curve_lut[c][i] = handle->curve_scratch[handle->exposure_lut[i]];
        }
    }
    for (int y = 0; y < 256; y++) {
        float t = (float)y / 255.0f;
        for (int c = 0; c < FILM_CHANNELS; c++) {
            float tint = (1.0f - t) * preset->shadow_tint[c] + t * preset->highlight_tint[c];
            handle->tint_q8[c][y] = (int16_t)lroundf(tint * Q8_ONE);
        }
        float amp = preset->grain * 255.0f * (GRAIN_BASE + GRAIN_MID * t * (1.0f - t));
        handle->grain_amp_q12[y] = (uint16_t)lroundf(amp / GRAIN_NOISE_STD * 4096.0f);
    }
}

/* 把 ((i - center) / radius)² 写成 Q14 表，超过 1 的截到 1（对漏光而言即"光斑外"） */
static void build_radial_axis(uint16_t *table, uint16_t count, float center, float radius, bool clamp_to_one)
{
    const float one = (float)(1 << FILM_RADIAL_Q);
    for (uint16_t i = 0; i < count; i++) {
        float n = ((float)i - center) / radius;
        float v = n * n;
        if (clamp_to_one && v > 1.0f) {
            v = 1.0f;
        }
        table[i] = (uint16_t)lroundf(v * one);
    }
}

/* 强度为 0 时整张表都是 1.0（32768），收尾里照常相乘也不改变结果 */
static void build_vignette_tables(film_filter_handle_t handle, float strength, uint16_t width, uint16_t height)
{
    for (int i = 0; i < FILM_RADIAL_LUT_SIZE; i++) {
        float r = sqrtf((float)i * 2.0f / (float)(FILM_RADIAL_LUT_SIZE - 1));
        float d = clamp01((r - VIGNETTE_INNER) / VIGNETTE_SPAN);
        float mask = 1.0f - strength * powf(d, VIGNETTE_EXPONENT);
        handle->vignette_q15[i] = (uint16_t)lroundf(mask * 32768.0f);
    }
    /* 原型以 w/2、h/2 为中心（不是 (w-1)/2），这里保持一致 */
    build_radial_axis(handle->vignette_col, width, width / 2.0f, width / 2.0f, false);
    build_radial_axis(handle->vignette_row, height, height / 2.0f, height / 2.0f, false);
}

/* 由种子决定从哪一侧漏光、光斑中心的高度 */
static void build_leak_axes(film_filter_handle_t handle, uint32_t seed, uint16_t width, uint16_t height)
{
    bool right = film_hash32(seed ^ 0xA5A5A5A5u) & 1u;
    float u = (float)(film_hash32(seed + 0x9E3779B9u) >> 8) / 16777216.0f;
    float cx = width * (right ? LEAK_CENTER_RIGHT : LEAK_CENTER_LEFT);
    float cy = height * (LEAK_CENTER_Y_MIN + LEAK_CENTER_Y_RANGE * u);
    build_radial_axis(handle->leak_col, width, cx, width * LEAK_RADIUS_X, true);
    build_radial_axis(handle->leak_row, height, cy, height * LEAK_RADIUS_Y, true);
}

/* 按缓存键决定要重建哪些表 */
static void prepare_tables(film_filter_handle_t handle, const film_preset_t *preset, film_id_t film,
                           int16_t ev_centi, const film_develop_ctx_t *ctx)
{
    uint16_t width = ctx->image->width;
    uint16_t height = ctx->image->height;
    bool resized = width != handle->cached_width || height != handle->cached_height;

    if (resized || handle->cached_film != (int16_t)film) {
        build_tone_tables(handle, preset, ev_centi);
        build_vignette_tables(handle, preset->vignette, width, height);
        if (ctx->halation) {
            film_halation_prepare(handle, &ctx->grid, width);
        }
        handle->cached_film = (int16_t)film;
        handle->cached_ev_centi = ev_centi;
        handle->cached_width = width;
        handle->cached_height = height;
        /* 换了胶卷或尺寸，上一帧的红晕遮罩不能再用 */
        handle->halation_valid = false;
    } else if (handle->cached_ev_centi != ev_centi) {
        /* 只调了曝光：曲线重建即可，红晕遮罩晚一帧跟上 */
        build_tone_tables(handle, preset, ev_centi);
        handle->cached_ev_centi = ev_centi;
    }
    if (resized) {
        handle->leak_cached = false;
    }
    if (ctx->leak && (!handle->leak_cached || handle->cached_leak_seed != ctx->seed)) {
        build_leak_axes(handle, ctx->seed, width, height);
        handle->leak_cached = true;
        handle->cached_leak_seed = ctx->seed;
    }
}

/* ---------- 按遍分发 ---------- */

typedef void (*film_rows_fn_t)(const film_develop_ctx_t *ctx, uint16_t begin, uint16_t end);

typedef struct {
    film_rows_fn_t fn;
    const film_develop_ctx_t *ctx;
    uint16_t begin;
    uint16_t end;
} rows_job_t;

static void rows_job_run(void *arg)
{
    const rows_job_t *job = arg;
    job->fn(job->ctx, job->begin, job->end);
}

/*
 * 处理 [0, count) 行：有工作任务时调用者做上半段、工作任务做下半段，返回前两段都已完成。
 * 分界点向下对齐到 align 行（红晕第一遍要求两段落在不同的网格行上）。
 */
static void run_rows(const film_develop_ctx_t *ctx, film_rows_fn_t fn, uint16_t count, uint16_t align)
{
    film_worker_t *worker = ctx->filter->worker;
    uint16_t split = (uint16_t)(count / 2 / align * align);
    if (worker == NULL || count < FILM_PARALLEL_MIN_ROWS || split == 0) {
        fn(ctx, 0, count);
        return;
    }
    rows_job_t job = { .fn = fn, .ctx = ctx, .begin = split, .end = count };
    film_worker_start(worker, rows_job_run, &job);
    fn(ctx, 0, split);
    film_worker_wait(worker);
}

/* ---------- 入口 ---------- */

static esp_err_t validate_request(film_filter_handle_t handle, const film_develop_params_t *params,
                                  const film_image_t *image)
{
    if (handle == NULL || params == NULL || image == NULL || image->pixels == NULL ||
            image->width == 0 || image->height == 0 || film_preset_get(params->film) == NULL ||
            image->stride < (size_t)image->width * FILM_CHANNELS ||
            (image->order != FILM_ORDER_RGB && image->order != FILM_ORDER_BGR)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (image->width > handle->max_width || image->height > handle->max_height) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t film_filter_develop(film_filter_handle_t handle, const film_develop_params_t *params,
                              const film_image_t *image)
{
    esp_err_t err = validate_request(handle, params, image);
    if (err != ESP_OK) {
        return err;
    }
    const film_preset_t *preset = film_preset_get(params->film);
    float ev = params->exposure_ev;
    ev = ev < FILM_EXPOSURE_EV_MIN ? FILM_EXPOSURE_EV_MIN : (ev > FILM_EXPOSURE_EV_MAX ? FILM_EXPOSURE_EV_MAX : ev);
    int16_t ev_centi = (int16_t)lroundf(ev * 100.0f);
    film_develop_ctx_t ctx = {
        .filter = handle,
        .image = image,
        .seed = params->seed,
        .bgr = image->order == FILM_ORDER_BGR,
        .exposure = ev_centi != 0,
    };

    if (preset->pixel) {
        /* 像素风不走曲线表；曝光表很小，每次按需重建，不占用彩色管线的缓存键 */
        if (ctx.exposure) {
            build_exposure_lut(handle->exposure_lut, ev_centi);
        }
        handle->cached_ev_centi = EV_CENTI_INVALID;
        handle->halation_valid = false;
        film_pixel_prepare(&ctx);
        run_rows(&ctx, film_pixel_cell_rows, ctx.pixel_rows, 1);
        run_rows(&ctx, film_pixel_upsample_rows, image->height, 1);
        return ESP_OK;
    }

    ctx.sat_q8 = (int32_t)lroundf(preset->saturation * Q8_ONE);
    ctx.mono = preset->mono;
    ctx.halation = preset->halation > 0.0f;
    ctx.leak = params->light_leak;
    ctx.grain = params->grain && preset->grain > 0.0f;
    ctx.grid = film_halation_grid_for(image->width, image->height);
    prepare_tables(handle, preset, params->film, ev_centi, &ctx);

    if (!ctx.halation) {
        run_rows(&ctx, film_color_fused_rows, image->height, 1);
        return ESP_OK;
    }

    for (int c = 0; c < FILM_CHANNELS; c++) {
        /* 遮罩满值 255 × MASK_SCALE 对应加 color × halation 个色阶（Q8 下再乘 256） */
        float full = 255.0f * FILM_HALATION_MASK_SCALE;
        ctx.halation_k_q16[c] = (int32_t)lroundf(s_halation_color[c] * preset->halation / full * 65536.0f);
    }
    size_t mask_bytes = (size_t)ctx.grid.width * ctx.grid.height * sizeof(uint32_t);

    if (params->reuse_halation && handle->halation_valid) {
        /* 一遍：读 halation_mask（上一帧已模糊），同时把本帧遮罩累加进 halation_next */
        ctx.halation_accum = handle->halation_next;
        memset(ctx.halation_accum, 0, mask_bytes);
        run_rows(&ctx, film_color_reuse_rows, image->height, ctx.grid.factor);
        uint32_t *done = handle->halation_next;
        handle->halation_next = handle->halation_mask;
        handle->halation_mask = done;
        film_halation_resolve(handle, &ctx.grid, image->width, image->height);
        return ESP_OK;
    }

    ctx.halation_accum = handle->halation_mask;
    memset(ctx.halation_accum, 0, mask_bytes);
    run_rows(&ctx, film_color_tone_rows, image->height, ctx.grid.factor);
    film_halation_resolve(handle, &ctx.grid, image->width, image->height);
    run_rows(&ctx, film_color_finish_rows, image->height, 1);
    /* 遮罩保留到下一次冲洗，供取景的快速红晕使用 */
    handle->halation_valid = true;
    return ESP_OK;
}

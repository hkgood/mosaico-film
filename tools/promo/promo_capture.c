/*
 * 宣传片素材录制：用 PC 平台服务驱动完整的 film_app，按分镜录下每段界面（480×480 PPM 序列），
 * 并记下每段里的音效、触摸、按键发生在第几帧，供 Remotion 合成画面、promo_audio.py 对齐音效。
 *
 *   film_promo_capture <场景目录> <数据目录（须为空）> <输出目录>
 *
 * 场景目录由 prep_scenes.py 生成：<名字>.ppm（1350×1800，拍照用）与 <名字>_vf.ppm（960×1280，取景平移用），
 * 都是正立的"取景方向"竖幅画面；喂给传感器前顺时针转 90°（传感器在机身里转了 90°，见 film_darkroom.c）。
 *
 * 时间：界面用虚拟时钟推进（每帧 1000/30 ms），录出来的动画帧帧均匀；
 * 真实时间至少跟上虚拟时间，让模拟平台里按真实时间走的冲洗进度、分享状态与画面节奏一致。
 * 录制工具单线程使用：全局 s_cap 只给音效回调找到当前录制段。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_private.h"
#include "cam_private.h"
#include "film_app.h"
#include "film_library.h"
#include "film_port_pc.h"
#include "ppm_io.h"

#define FPS            30
#define MAX_CLIPS      16
#define MAX_MARKS      2048
#define MAX_PRINTS     32
#define PATH_LEN       512
/* 取景平移窗口（从 _vf 场景里裁）与拍照窗口（从全尺寸场景里裁），取景方向的竖幅 3:4 */
#define VF_W           768
#define VF_H           1024
#define STILL_W        1080
#define STILL_H        1440
/* 手持的微小晃动（像素）与周期（帧） */
#define SHAKE_PX       5.0f
#define SHAKE_PERIOD   97.0f
#define WAIT_SHOT_FRAMES (40 * FPS)

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/** 一段录制里发生的事：触摸采样、按键、音效 */
typedef struct {
    int frame;
    char type[12];      /*!< "down" "move" "up" "key_down" "key_up" "sfx" */
    char name[12];      /*!< 音效名 */
    int x, y;
} mark_t;

typedef struct {
    char name[32];
    int frames;
    int n_marks;
    mark_t marks[MAX_MARKS];
} clip_t;

/** 导出给视频用的成片（真实冲洗管线的输出） */
typedef struct {
    char name[32];
    uint32_t id;
    int film;
    bool instant;
} print_t;

typedef struct {
    film_app_handle_t app;
    film_port_pc_handle_t port;
    film_port_t table;          /*!< 平台服务表的拷贝：只替换 feedback，录下音效 */
    const film_port_t *inner;
    uint16_t *frame565;
    uint8_t *frame888;
    const char *scene_dir;
    const char *out_dir;
    char root[PATH_LEN];
    uint64_t t0_real;
    uint64_t vframe;            /*!< 虚拟时钟：已推进的帧数 */
    clip_t *clips;
    int n_clips;
    clip_t *rec;                /*!< 正在录制的段，NULL 表示只推进不录 */
    print_t prints[MAX_PRINTS];
    int n_prints;
    /* 取景平移：从 scene 里裁 win 大小的窗口，窗口左上角在 pan_frames 帧内从 p0 移到 p1 */
    ppm_image_t scene;
    int win_w, win_h;
    float px0, py0, px1, py1;
    uint64_t pan_f0;
    int pan_frames;
    uint8_t *crop;
} cap_t;

static cap_t *s_cap;

static uint64_t real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static uint32_t vnow(const cap_t *c)
{
    return (uint32_t)(1000 + c->vframe * 1000 / FPS);
}

static void mark(cap_t *c, const char *type, const char *name, int x, int y)
{
    clip_t *r = c->rec;
    if (!r || r->n_marks >= MAX_MARKS) {
        return;
    }
    mark_t *m = &r->marks[r->n_marks++];
    m->frame = r->frames;   /* 下一张写出的帧就是它生效后的画面 */
    snprintf(m->type, sizeof(m->type), "%s", type);
    snprintf(m->name, sizeof(m->name), "%s", name ? name : "");
    m->x = x;
    m->y = y;
}

static void record_feedback(void *ctx, film_feedback_t kind)
{
    static const char *const names[] = { "shutter", "detent", "click", "lever", "shake", "error" };
    if ((unsigned)kind < sizeof(names) / sizeof(names[0])) {
        mark(s_cap, "sfx", names[kind], 0, 0);
    }
    s_cap->inner->feedback(ctx, kind);
}

/* ---------------------------------------------------------------- 场景与取景平移 */

static void load_scene(cap_t *c, const char *name, bool viewfinder)
{
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s%s.ppm", c->scene_dir, name, viewfinder ? "_vf" : "");
    free(c->scene.pixels);
    c->scene.pixels = NULL;
    CHECK(ppm_read(path, &c->scene));
    c->win_w = viewfinder ? VF_W : STILL_W;
    c->win_h = viewfinder ? VF_H : STILL_H;
    CHECK(c->scene.width >= c->win_w && c->scene.height >= c->win_h);
    const float cx = (float)(c->scene.width - c->win_w) / 2;
    const float cy = (float)(c->scene.height - c->win_h) / 2;
    c->px0 = c->px1 = cx;
    c->py0 = c->py1 = cy;
    c->pan_frames = 0;
}

/**
 * 取景窗口从场景内的相对位置 (fx0, fy0) 平移到 (fx1, fy1)（0..1，0.5 为居中），历时 frames 帧。
 */
static void pan(cap_t *c, float fx0, float fy0, float fx1, float fy1, int frames)
{
    const float mx = (float)(c->scene.width - c->win_w);
    const float my = (float)(c->scene.height - c->win_h);
    c->px0 = fx0 * mx;
    c->py0 = fy0 * my;
    c->px1 = fx1 * mx;
    c->py1 = fy1 * my;
    c->pan_f0 = c->vframe;
    c->pan_frames = frames;
}

static void update_sensor(cap_t *c)
{
    if (!c->scene.pixels) {
        return;
    }
    float t = c->pan_frames > 0 ? (float)(c->vframe - c->pan_f0) / (float)c->pan_frames : 1.0f;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    t = t * t * (3 - 2 * t);
    /* 只有取景窗口才加手持晃动；拍照窗口固定，成片构图可控 */
    const float shake = c->win_w == VF_W ? SHAKE_PX : 0.0f;
    const float ph = (float)c->vframe / SHAKE_PERIOD * 6.2831853f;
    float x = c->px0 + (c->px1 - c->px0) * t + shake * sinf(ph);
    float y = c->py0 + (c->py1 - c->py0) * t + shake * 0.6f * sinf(ph * 1.7f + 1.0f);
    const int mx = c->scene.width - c->win_w;
    const int my = c->scene.height - c->win_h;
    int ix = (int)lroundf(x), iy = (int)lroundf(y);
    ix = ix < 0 ? 0 : (ix > mx ? mx : ix);
    iy = iy < 0 ? 0 : (iy > my ? my : iy);
    /* 顺时针转 90° 成传感器画面（宽 = 窗口高）：传感器 (sx, sy) 取窗口里的 (sy, win_h-1-sx) */
    const int sw = c->win_h, sh = c->win_w;
    for (int sy = 0; sy < sh; ++sy) {
        uint8_t *dst = c->crop + (size_t)sy * sw * 3;
        for (int sx = 0; sx < sw; ++sx) {
            const uint8_t *src = c->scene.pixels + ((size_t)(iy + c->win_h - 1 - sx) * c->scene.width + ix + sy) * 3;
            memcpy(dst + (size_t)sx * 3, src, 3);
        }
    }
    /* 冲洗任务进行中会拒绝替换：这期间取景本来就定格在刚拍的画面上 */
    (void)film_port_pc_set_sensor(c->port, c->crop, sw, sh);
}

/* ---------------------------------------------------------------- 时钟与录制 */

static void write_frame(cap_t *c)
{
    gfx_canvas_t canvas = {
        .pixels = c->frame565, .width = FILM_APP_SCREEN_W, .height = FILM_APP_SCREEN_H,
        .stride = FILM_APP_SCREEN_W, .clip = { 0, 0, FILM_APP_SCREEN_W, FILM_APP_SCREEN_H },
    };
    film_app_render(c->app, &canvas);
    for (int i = 0; i < FILM_APP_SCREEN_W * FILM_APP_SCREEN_H; ++i) {
        const uint16_t p = c->frame565[i];
        c->frame888[i * 3 + 0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
        c->frame888[i * 3 + 1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
        c->frame888[i * 3 + 2] = (uint8_t)((p & 31) * 255 / 31);
    }
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s/%04d.ppm", c->out_dir, c->rec->name, c->rec->frames);
    const ppm_image_t img = { .pixels = c->frame888, .width = FILM_APP_SCREEN_W, .height = FILM_APP_SCREEN_H };
    CHECK(ppm_write(path, &img));
    c->rec->frames++;
}

/** 推进一帧：真实时间跟上虚拟时钟 → 换取景画面 → 推进界面 → 录制中则写出这一帧 */
static void tick(cap_t *c)
{
    c->vframe++;
    const uint64_t target = c->t0_real + c->vframe * 1000 / FPS;
    for (uint64_t now = real_ms(); now < target; now = real_ms()) {
        const struct timespec ts = { 0, (long)(target - now) * 1000000L };
        nanosleep(&ts, NULL);
    }
    update_sensor(c);
    film_app_step(c->app, vnow(c));
    if (c->rec) {
        write_frame(c);
    }
}

static void wait_frames(cap_t *c, int frames)
{
    for (int i = 0; i < frames; ++i) {
        tick(c);
    }
}

static void rec_begin(cap_t *c, const char *name)
{
    CHECK(!c->rec && c->n_clips < MAX_CLIPS);
    clip_t *r = &c->clips[c->n_clips++];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", c->out_dir, name);
    mkdir(path, 0755);
    c->rec = r;
    printf("rec %s ...\n", name);
    fflush(stdout);
}

static void rec_end(cap_t *c)
{
    printf("rec %s: %d frames, %d marks\n", c->rec->name, c->rec->frames, c->rec->n_marks);
    fflush(stdout);
    c->rec = NULL;
}

/* ---------------------------------------------------------------- 操作 */

static void touch(cap_t *c, int x, int y, bool pressed, const char *phase)
{
    mark(c, phase, NULL, x, y);
    film_app_pointer(c->app, x, y, pressed, vnow(c));
    tick(c);
}

static void tap(cap_t *c, int x, int y, int hold_frames)
{
    touch(c, x, y, true, "down");
    for (int i = 0; i < hold_frames; ++i) {
        touch(c, x, y, true, "move");
    }
    touch(c, x, y, false, "up");
}

/** 匀速拖动 frames 帧，起止各停一帧 */
static void drag(cap_t *c, int x0, int y0, int x1, int y1, int frames)
{
    touch(c, x0, y0, true, "down");
    for (int i = 1; i <= frames; ++i) {
        float t = (float)i / (float)frames;
        t = t * t * (3 - 2 * t);
        touch(c, x0 + (int)lroundf((float)(x1 - x0) * t), y0 + (int)lroundf((float)(y1 - y0) * t), true, "move");
    }
    touch(c, x1, y1, false, "up");
}

static void key(cap_t *c, bool pressed)
{
    mark(c, pressed ? "key_down" : "key_up", NULL, 0, 0);
    film_app_key(c->app, FILM_KEY_SHUTTER, pressed, vnow(c));
    tick(c);
}

static void key_click(cap_t *c, int hold_frames)
{
    key(c, true);
    wait_frames(c, hold_frames);
    key(c, false);
}

static bool camera_idle(const film_app_t *app)
{
    const cam_state_t *cam = &app->cam;
    return app->screen == SCR_CAMERA && cam->shot == SHOT_IDLE && cam->overlay == CAM_OVL_NONE &&
           !cam->skin_swapping && cam->picker_pull == 0.0f;
}

/** 等相册里多出一张、并且相机回到可拍状态（SX-70 显影层会在停留后自己收起） */
static void wait_shot_done(cap_t *c, size_t before)
{
    for (int i = 0; i < WAIT_SHOT_FRAMES; ++i) {
        if (film_library_count(c->app->library) > before && camera_idle(c->app)) {
            return;
        }
        tick(c);
    }
    fprintf(stderr, "timeout waiting for shot (library %zu)\n", film_library_count(c->app->library));
    exit(1);
}

static void wait_body(cap_t *c, bool instant)
{
    if ((c->app->settings.instant != 0) != instant) {
        cam_switch_body(c->app, instant);
    }
    for (int i = 0; i < 4 * FPS && (c->app->cam.skin_swapping || (c->app->settings.instant != 0) != instant); ++i) {
        tick(c);
    }
    CHECK((c->app->settings.instant != 0) == instant);
    wait_frames(c, 10);
}

/** 把相册里第 index 张照片记到导出清单（胶卷与相纸按照片自己的元数据） */
static uint32_t remember_print(cap_t *c, size_t index, const char *label)
{
    const film_photo_t *photo = film_library_get(c->app->library, index);
    CHECK(photo && c->n_prints < MAX_PRINTS);
    print_t *p = &c->prints[c->n_prints++];
    snprintf(p->name, sizeof(p->name), "%s", label);
    p->id = photo->id;
    p->film = photo->film;
    p->instant = (photo->flags & FILM_PHOTO_INSTANT) != 0;
    printf("print %s: photo %u film %d%s\n", label, (unsigned)p->id, p->film, p->instant ? " instant" : "");
    return p->id;
}

/** 不录制地拍一张：场景、胶卷、机身都按参数来，成片记到导出清单 */
static uint32_t prep_shot(cap_t *c, const char *scene, float fx, float fy, int film, bool instant, const char *label)
{
    wait_body(c, instant);
    load_scene(c, scene, false);
    pan(c, fx, fy, fx, fy, 0);
    cam_select_film(c->app, film);
    wait_frames(c, 6);
    const size_t before = film_library_count(c->app->library);
    key_click(c, 3);
    wait_shot_done(c, before);
    return remember_print(c, 0, label);
}

/** 相册网格里第 index 张（0 为最新，未滚动）的中心 */
static void cell_center(size_t index, int *x, int *y)
{
    static const int col_x[3] = { 79, 240, 401 };
    *x = col_x[index % 3];
    *y = HEADER_H + 3 + (int)(index / 3) * 122 + 60;
}

static size_t index_of(cap_t *c, uint32_t id)
{
    size_t index = 0;
    CHECK(film_library_find(c->app->library, id, &index));
    return index;
}

/* ---------------------------------------------------------------- 导出 */

static void export_prints(cap_t *c)
{
    char dir[PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/prints", c->out_dir);
    mkdir(dir, 0755);
    for (int i = 0; i < c->n_prints; ++i) {
        char from[PATH_LEN], to[PATH_LEN];
        film_library_path(c->root, c->prints[i].id, FILM_FILE_JPEG, from, sizeof(from));
        snprintf(to, sizeof(to), "%s/%s.ppm", dir, c->prints[i].name);
        ppm_image_t img;
        CHECK(ppm_read(from, &img));   /* 模拟器里成片是写在 .JPG 名下的 PPM */
        CHECK(ppm_write(to, &img));
        free(img.pixels);
    }
}

static void write_manifest(cap_t *c)
{
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/manifest.json", c->out_dir);
    FILE *f = fopen(path, "w");
    CHECK(f);
    fprintf(f, "{\n  \"fps\": %d,\n  \"clips\": {\n", FPS);
    for (int i = 0; i < c->n_clips; ++i) {
        const clip_t *r = &c->clips[i];
        fprintf(f, "    \"%s\": {\"frames\": %d, \"marks\": [", r->name, r->frames);
        for (int m = 0; m < r->n_marks; ++m) {
            const mark_t *k = &r->marks[m];
            fprintf(f, "%s\n      {\"frame\": %d, \"type\": \"%s\", \"name\": \"%s\", \"x\": %d, \"y\": %d}",
                    m ? "," : "", k->frame, k->type, k->name, k->x, k->y);
        }
        fprintf(f, "%s]}%s\n", r->n_marks ? "\n    " : "", i + 1 < c->n_clips ? "," : "");
    }
    fprintf(f, "  },\n  \"prints\": [\n");
    for (int i = 0; i < c->n_prints; ++i) {
        const print_t *p = &c->prints[i];
        fprintf(f, "    {\"name\": \"%s\", \"film\": %d, \"filmName\": \"%s\", \"instant\": %s}%s\n", p->name, p->film,
                film_info(p->film)->name, p->instant ? "true" : "false", i + 1 < c->n_prints ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

/* ---------------------------------------------------------------- 分镜 */

/** 相册素材：同一张街景过 8 款胶卷（做对比），再拍一组不同场景的照片填满相册 */
static uint32_t prep_album(cap_t *c)
{
    for (int film = 0; film < FILM_ID_COUNT; ++film) {
        char label[32];
        snprintf(label, sizeof(label), "films_%d", film);
        prep_shot(c, "zurich", 0.5f, 0.5f, film, false, label);
    }
    prep_shot(c, "beach", 0.5f, 0.6f, FILM_ID_GOLD, true, "sx_beach");
    prep_shot(c, "sunflower", 0.5f, 0.5f, FILM_ID_GREEN, false, "sunflower");
    prep_shot(c, "arles", 0.5f, 0.4f, FILM_ID_FADED, false, "arles");
    const uint32_t tokyo = prep_shot(c, "tokyo", 0.5f, 0.5f, FILM_ID_NIGHT, false, "tokyo");
    prep_shot(c, "cafe", 0.5f, 0.5f, FILM_ID_PORTRA, true, "sx_cafe");
    prep_shot(c, "beach", 0.5f, 0.55f, FILM_ID_GOLD, false, "beach");
    prep_shot(c, "neon", 0.5f, 0.5f, FILM_ID_CROSS, false, "neon");
    prep_shot(c, "sunflower", 0.3f, 0.5f, FILM_ID_PORTRA, true, "sx_sunflower");
    prep_shot(c, "arles", 0.5f, 0.6f, FILM_ID_BW, false, "arles_bw");
    return tokyo;
}

/*
 * 换卷的节拍：每拍 15 帧，一次横滑正好换一卷（滑 100 px > FILM_SWIPE_PX，且不到两卷），
 * drag 占 8 帧（按下 + 6 + 抬起），再停 7 帧。视频里的成片按录到的拨盘声逐卷切换。
 */
#define FILMS_SWIPE_PX      100
#define FILMS_SWIPE_FRAMES  6
#define FILMS_REST_FRAMES   7

/** M6：取景平移；从 GOLD 起一拍一卷横滑，8 款胶卷依次过一遍 */
static void clip_m6_films(cap_t *c)
{
    wait_body(c, false);
    cam_select_film(c->app, FILM_ID_GOLD);
    load_scene(c, "zurich", true);
    pan(c, 0.2f, 0.5f, 0.8f, 0.45f, 10 * FPS);
    wait_frames(c, 20);
    rec_begin(c, "m6_films");
    wait_frames(c, 12);
    for (int i = 0; i < FILM_ID_COUNT - 1; ++i) {
        drag(c, 330, 190, 330 - FILMS_SWIPE_PX, 190, FILMS_SWIPE_FRAMES);
        wait_frames(c, FILMS_REST_FRAMES);
    }
    CHECK(c->app->settings.film == FILM_ID_COUNT - 1);
    wait_frames(c, 20);
    rec_end(c);
}

/** M6：按下红键 → 快门帘 → 定格原片 → 小样淡入 → DEVELOPED → 飞进计数窗 */
static void clip_m6_shot(cap_t *c)
{
    setenv("FILM_SIM_DEVELOP_DELAY_MS", "30", 1);
    cam_select_film(c->app, FILM_ID_GOLD);
    load_scene(c, "arles", true);
    pan(c, 0.5f, 0.35f, 0.5f, 0.45f, 6 * FPS);
    wait_frames(c, 30);
    const size_t before = film_library_count(c->app->library);
    rec_begin(c, "m6_shot");
    wait_frames(c, 20);
    key_click(c, 4);
    for (int i = 0; i < 9 * FPS && !camera_idle(c->app); ++i) {
        tick(c);
    }
    wait_frames(c, 15);
    rec_end(c);
    wait_shot_done(c, before);
    setenv("FILM_SIM_DEVELOP_DELAY_MS", "0", 1);
}

/** 顶边下拉机身选择 → 点 SX-70 → 皮革合拢换机身 */
static void clip_picker(cap_t *c)
{
    load_scene(c, "cafe", true);
    pan(c, 0.4f, 0.5f, 0.6f, 0.5f, 8 * FPS);
    wait_frames(c, 10);
    rec_begin(c, "picker");
    wait_frames(c, 10);
    drag(c, 240, 8, 240, 290, 14);
    wait_frames(c, 20);
    tap(c, 352, 162, 3);
    for (int i = 0; i < 3 * FPS && (c->app->cam.skin_swapping || !c->app->settings.instant); ++i) {
        tick(c);
    }
    wait_frames(c, 36);
    rec_end(c);
}

/** SX-70：按下红键 → 相纸吐出 → 雾层下慢慢显影 → 停留后回到取景 */
static void clip_sx_develop(cap_t *c)
{
    setenv("FILM_SIM_DEVELOP_DELAY_MS", "70", 1);
    cam_select_film(c->app, FILM_ID_GOLD);
    load_scene(c, "coffee", true);
    pan(c, 0.5f, 0.5f, 0.52f, 0.48f, 6 * FPS);
    wait_frames(c, 30);
    const size_t before = film_library_count(c->app->library);
    rec_begin(c, "sx_develop");
    wait_frames(c, 15);
    key_click(c, 4);
    for (int i = 0; i < 14 * FPS && !camera_idle(c->app); ++i) {
        tick(c);
    }
    wait_frames(c, 10);
    rec_end(c);
    wait_shot_done(c, before);
    setenv("FILM_SIM_DEVELOP_DELAY_MS", "0", 1);
}

/** 暗房：相纸堆 → 相册 → 点开夜景 → 重新冲洗，滚筒连换几卷 */
static void clip_redevelop(cap_t *c, uint32_t tokyo)
{
    wait_frames(c, 10);
    rec_begin(c, "redevelop");
    tap(c, SX_STACK_X, SX_STACK_Y, 2);
    wait_frames(c, 40);
    int x, y;
    cell_center(index_of(c, tokyo), &x, &y);
    tap(c, x, y, 2);
    wait_frames(c, 30);
    tap(c, 164, 422, 2);   /* REDEVELOP */
    for (int i = 0; i < 6 * FPS && (c->app->redev.request_in_flight || c->app->redev.developing); ++i) {
        tick(c);
    }
    wait_frames(c, 30);
    static const int steps[] = { 1, 1, 1 };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        drag(c, 340, 380, 220, 380, 10);
        for (int k = 0; k < 6 * FPS && c->app->redev.request_in_flight; ++k) {
            tick(c);
        }
        wait_frames(c, 24);
    }
    rec_end(c);
}

/** 回到相册多选 3 张 → SEND TO PHONE → 二维码 → 手机打开并保存 */
static void clip_share(cap_t *c)
{
    tap(c, HEADER_BACK_X + 18, HEADER_BACK_Y + 18, 2);
    wait_frames(c, 20);
    tap(c, HEADER_BACK_X + 18, HEADER_BACK_Y + 18, 2);
    wait_frames(c, 30);
    CHECK(c->app->screen == SCR_ALBUM);
    rec_begin(c, "share");
    wait_frames(c, 10);
    tap(c, 440, 32, 2);    /* SELECT */
    wait_frames(c, 10);
    static const size_t picks[] = { 1, 3, 4 };
    for (size_t i = 0; i < sizeof(picks) / sizeof(picks[0]); ++i) {
        char label[32];
        snprintf(label, sizeof(label), "shared_%zu", i);
        remember_print(c, picks[i], label);
        int x, y;
        cell_center(picks[i], &x, &y);
        tap(c, x, y, 2);
        wait_frames(c, 8);
    }
    wait_frames(c, 10);
    tap(c, 344, 422, 3);   /* SEND TO PHONE */
    wait_frames(c, 9 * FPS);
    rec_end(c);
    tap(c, 345, 422, 2);   /* DONE（同网分享时在右半边） */
    wait_frames(c, 20);
    CHECK(c->app->screen == SCR_ALBUM);
}

/** 收尾：回到 M6 取景，海边日落缓慢平移 */
static void clip_finale(cap_t *c)
{
    key_click(c, 2);       /* 暗房里按红键回到取景 */
    wait_frames(c, 20);
    wait_body(c, false);
    cam_select_film(c->app, FILM_ID_GOLD);
    load_scene(c, "beach", true);
    pan(c, 0.3f, 1.0f, 0.7f, 0.9f, 7 * FPS);  /* 太阳在画面下部：窗口贴着下沿慢慢上移 */
    wait_frames(c, 10);
    rec_begin(c, "finale");
    wait_frames(c, 6 * FPS);
    rec_end(c);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 4);
    static cap_t cap;
    cap_t *c = &cap;
    s_cap = c;
    c->scene_dir = argv[1];
    c->out_dir = argv[3];
    mkdir(c->out_dir, 0755);
    snprintf(c->root, sizeof(c->root), "%s/DCIM/MOSAICO", argv[2]);
    c->clips = calloc(MAX_CLIPS, sizeof(clip_t));
    c->frame565 = malloc(sizeof(uint16_t) * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
    c->frame888 = malloc((size_t)3 * FILM_APP_SCREEN_W * FILM_APP_SCREEN_H);
    c->crop = malloc((size_t)3 * STILL_W * STILL_H);
    CHECK(c->clips && c->frame565 && c->frame888 && c->crop);

    setenv("FILM_SIM_DEVELOP_DELAY_MS", "0", 1);
    char sensor[PATH_LEN];
    snprintf(sensor, sizeof(sensor), "%s/zurich_vf.ppm", c->scene_dir);
    const film_port_pc_config_t pc = { .data_dir = argv[2], .sensor_ppm = sensor, .assets_path = FILM_TEST_ASSETS };
    CHECK(film_port_pc_create(&pc, &c->port) == ESP_OK);
    c->inner = film_port_pc_table(c->port);
    c->table = *c->inner;
    c->table.feedback = record_feedback;
    const film_app_config_t ac = { .port = &c->table, .seed = 2026 };
    CHECK(film_app_create(&ac, &c->app) == ESP_OK);
    CHECK(film_library_count(c->app->library) == 0);   /* 要求空的数据目录 */
    c->t0_real = real_ms();
    wait_frames(c, 20);

    const uint32_t tokyo = prep_album(c);
    clip_m6_films(c);
    clip_m6_shot(c);
    clip_picker(c);
    clip_sx_develop(c);
    clip_redevelop(c, tokyo);
    clip_share(c);
    clip_finale(c);

    export_prints(c);
    write_manifest(c);
    film_app_delete(c->app);
    film_port_pc_delete(c->port);
    free(c->scene.pixels);
    printf("film_promo_capture: OK\n");
    return 0;
}

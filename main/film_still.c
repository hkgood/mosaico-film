#include "film_still.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "film_balance.h"
#include "freertos/FreeRTOS.h"
#include "linux/videodev2.h"

static const char *TAG = "film_still";

#define STILL_BUFFERS           1
#define STILL_BUFFER_BYTES      (1024 * 1024)   /*!< JPEG 实测 0.55–0.73 MB */
#define STILL_FRAME_TIMEOUT_MS  2000
#define STILL_MAX_FRAMES        8               /*!< 最多看这么多帧还没取到就放弃 */
#define STILL_TAKE_VALID_FRAME  2               /*!< 取第几张有效帧（第 1 张偏暗，第 2 张曝光正确） */
#define STILL_COPY_CAPS         (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SCCB_READ_ATTEMPTS      3
#define JPEG_EOI_SEARCH_BYTES   64              /*!< DVP 帧尾可能带几个填充字节，EOI 在最后这么多字节里 */

/* OV3640 曝光寄存器（传感器驱动没有曝光接口，按数据手册直接访问） */
#define OV3640_REG_GAIN_H       0x3000
#define OV3640_REG_GAIN_L       0x3001
#define OV3640_REG_AEC_H        0x3002
#define OV3640_REG_AEC_L        0x3003
#define OV3640_REG_AUTO         0x3013
#define OV3640_AUTO_AEC_AGC     0x05    /*!< bit0 自动曝光，bit2 自动增益 */
#define OV3640_REG_HTS_H        0x3028
#define OV3640_REG_HTS_L        0x3029
#define OV3640_REG_VTS_H        0x302A
#define OV3640_REG_VTS_L        0x302B
#define OV3640_REG_EXTRA_H      0x302D
#define OV3640_REG_EXTRA_L      0x302E
#define OV3640_VTS_MARGIN       4
/*
 * 拍照模式比取景暗：取景是合并读出，同样的曝光时间信号更强。
 * 换算到拍照模式时把曝光行数再乘上这个系数补回来。
 * 实测：不补时暗房要把原片 G 提亮 2.4~3.4 倍，补 2.0 后还剩 1.30 倍；
 * 取 2.4 让暗房只剩约 1.1 倍的小幅提亮，给高光留一点余量（成片亮度由暗房按取景匹配，不随此值变化）。
 */
#define STILL_EXPOSURE_BOOST    2.4f
#define OV3640_REG_AWB_CTRL     0x332B
#define OV3640_AWB_MANUAL_BIT   0x08
/* 白平衡 R/G/B 增益（0x40 = 1.0）；0x332B bit3 置位后由这三个寄存器决定，用法同 ArduCAM OV3640 光源模式 */
#define OV3640_REG_AWB_GAIN_R   0x33A7
#define OV3640_AWB_CHANNELS     3
#define OV3640_AWB_UNITY        0x40
#define OV3640_AWB_MIN          0x10
#define OV3640_AWB_MAX          0xFF
/* 开机后第一张的增益：室内灯光下实测接近中性的值（与 ArduCAM 室内灯预设 44/40/70 相近） */
#define STILL_AWB_DEFAULT_R     0x4C
#define STILL_AWB_DEFAULT_B     0x6C
/*
 * 每次只修正残差的这个次方。增益之后还有色彩校正矩阵，通道互相牵连且很陡：
 * 实测原片红色约随增益的 2 次方变化，蓝色在中性附近约 4 次方，整步修正会来回振荡。
 */
#define STILL_AWB_LEARN_RATE    0.25f

/* 与 mosaico_module_camera 取景时写入的白平衡调校相同（格式切换会把它冲掉） */
static const struct {
    uint16_t reg;
    uint8_t value;
} k_awb_tuning[] = {
    {0x3317, 0x04}, {0x3316, 0xF8}, {0x3312, 0x26}, {0x3314, 0x42},
    {0x3313, 0x2B}, {0x3315, 0x42}, {0x3310, 0xD0}, {0x3311, 0xBD},
    {0x330C, 0x18}, {0x330D, 0x18}, {0x330E, 0x56}, {0x330F, 0x5C},
    {0x330B, 0x1C}, {0x3306, 0x5C}, {0x3307, 0x11}, {0x3308, 0x25},
};

typedef struct {
    uint8_t gain_h;
    uint8_t gain_l;
    uint8_t auto_ctrl;
    uint16_t exposure;      /*!< 曝光行数 */
    uint16_t hts;           /*!< 每行像素时钟 */
    uint16_t vts;           /*!< 每帧行数 */
} exposure_t;

/*
 * 学到的拍照白平衡增益：相机任务读（拍照时），暗房任务写（冲洗后反馈），用自旋锁保护。
 */
static film_still_awb_t s_awb = { .gain = { STILL_AWB_DEFAULT_R, OV3640_AWB_UNITY, STILL_AWB_DEFAULT_B } };
static portMUX_TYPE s_awb_lock = portMUX_INITIALIZER_UNLOCKED;

/* 一次拍照的原生流，只活在 film_still_capture 的调用期间 */
typedef struct {
    int fd;
    uint8_t *buffers[STILL_BUFFERS];
    size_t lengths[STILL_BUFFERS];
    uint32_t count;
    bool streaming;
    bool switched;
    esp_cam_sensor_format_t preview_format;
    exposure_t preview_exposure;
    film_still_awb_t awb;           /*!< 这次锁定的白平衡增益 */
} still_t;

static esp_err_t v4l2(int fd, unsigned long request, void *arg, const char *what)
{
    if (ioctl(fd, request, arg) != 0) {
        ESP_LOGE(TAG, "%s failed: errno=%d", what, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------- 传感器寄存器 */

static esp_err_t reg_access(int fd, uint32_t id, esp_cam_sensor_reg_val_t *reg)
{
    struct v4l2_ext_control control = { .id = id, .size = sizeof(*reg), .p_u8 = (uint8_t *)reg };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_ESP_CAM_IOCTL, .count = 1, .controls = &control,
    };
    return v4l2(fd, id == ESP_CAM_SENSOR_IOC_S_REG ? VIDIOC_S_EXT_CTRLS : VIDIOC_G_EXT_CTRLS, &controls, "sensor reg");
}

/*
 * SCCB 与触摸、IMU、模块探测共用主板 I2C。当前 IDF 的 i2c_master 在读传输被 NACK 后
 * 留下过期的读状态，紧接着的写传输会在中断里写坏内存；所以读失败要立刻重读，
 * 用一次成功的读把状态清掉，再继续后面的寄存器写。
 */
static esp_err_t reg_read(int fd, uint16_t addr, uint8_t *ret)
{
    esp_cam_sensor_reg_val_t reg = { .regaddr = addr };
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < SCCB_READ_ATTEMPTS && err != ESP_OK; ++attempt) {
        err = reg_access(fd, ESP_CAM_SENSOR_IOC_G_REG, &reg);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "read 0x%04x", addr);
    *ret = (uint8_t)reg.value;
    return ESP_OK;
}

static esp_err_t reg_write(int fd, uint16_t addr, uint8_t value)
{
    esp_cam_sensor_reg_val_t reg = { .regaddr = addr, .value = value };
    return reg_access(fd, ESP_CAM_SENSOR_IOC_S_REG, &reg);
}

static esp_err_t reg_read16(int fd, uint16_t hi, uint16_t lo, uint16_t *ret)
{
    uint8_t h = 0, l = 0;
    ESP_RETURN_ON_ERROR(reg_read(fd, hi, &h), TAG, "read hi");
    ESP_RETURN_ON_ERROR(reg_read(fd, lo, &l), TAG, "read lo");
    *ret = (uint16_t)((h << 8) | l);
    return ESP_OK;
}

static esp_err_t reg_write16(int fd, uint16_t hi, uint16_t lo, uint16_t value)
{
    ESP_RETURN_ON_ERROR(reg_write(fd, hi, (uint8_t)(value >> 8)), TAG, "write hi");
    return reg_write(fd, lo, (uint8_t)value);
}

static esp_err_t exposure_read(int fd, exposure_t *e)
{
    ESP_RETURN_ON_ERROR(reg_read(fd, OV3640_REG_GAIN_H, &e->gain_h), TAG, "gain h");
    ESP_RETURN_ON_ERROR(reg_read(fd, OV3640_REG_GAIN_L, &e->gain_l), TAG, "gain l");
    ESP_RETURN_ON_ERROR(reg_read(fd, OV3640_REG_AUTO, &e->auto_ctrl), TAG, "auto");
    ESP_RETURN_ON_ERROR(reg_read16(fd, OV3640_REG_AEC_H, OV3640_REG_AEC_L, &e->exposure), TAG, "aec");
    ESP_RETURN_ON_ERROR(reg_read16(fd, OV3640_REG_HTS_H, OV3640_REG_HTS_L, &e->hts), TAG, "hts");
    return reg_read16(fd, OV3640_REG_VTS_H, OV3640_REG_VTS_L, &e->vts);
}

/*
 * 把 from 模式的曝光换算到当前模式（to 为当前模式复位后的读数）：两种模式像素时钟相同，
 * 曝光时间 = 行数 × HTS，所以行数按 HTS 反比换算，再乘 boost；超出一帧的部分写成额外曝光行。
 */
static esp_err_t exposure_transfer(int fd, const exposure_t *from, const exposure_t *to, bool lock, float boost)
{
    const uint32_t base = to->hts ? (uint32_t)from->exposure * from->hts / to->hts : from->exposure;
    const uint32_t lines = (uint32_t)lroundf((float)base * boost);
    const uint32_t max_lines = to->vts > OV3640_VTS_MARGIN ? to->vts - OV3640_VTS_MARGIN : to->vts;
    const uint32_t extra = lines > max_lines ? lines - max_lines : 0;
    const uint8_t auto_ctrl = lock ? (uint8_t)(to->auto_ctrl & ~OV3640_AUTO_AEC_AGC)
                                   : (uint8_t)(to->auto_ctrl | OV3640_AUTO_AEC_AGC);
    ESP_RETURN_ON_ERROR(reg_write(fd, OV3640_REG_AUTO, auto_ctrl), TAG, "auto ctrl");
    ESP_RETURN_ON_ERROR(reg_write16(fd, OV3640_REG_AEC_H, OV3640_REG_AEC_L, (uint16_t)(lines - extra)), TAG, "aec");
    ESP_RETURN_ON_ERROR(reg_write16(fd, OV3640_REG_EXTRA_H, OV3640_REG_EXTRA_L, (uint16_t)extra), TAG, "extra");
    ESP_RETURN_ON_ERROR(reg_write(fd, OV3640_REG_GAIN_H, from->gain_h), TAG, "gain h");
    return reg_write(fd, OV3640_REG_GAIN_L, from->gain_l);
}

static esp_err_t replay_awb_tuning(int fd)
{
    for (size_t i = 0; i < sizeof(k_awb_tuning) / sizeof(k_awb_tuning[0]); ++i) {
        ESP_RETURN_ON_ERROR(reg_write(fd, k_awb_tuning[i].reg, k_awb_tuning[i].value), TAG, "awb 0x%04x",
                            k_awb_tuning[i].reg);
    }
    uint8_t ctrl = 0;
    ESP_RETURN_ON_ERROR(reg_read(fd, OV3640_REG_AWB_CTRL, &ctrl), TAG, "awb ctrl");
    return reg_write(fd, OV3640_REG_AWB_CTRL, (uint8_t)(ctrl & ~OV3640_AWB_MANUAL_BIT));
}

/* 拍照模式锁定为手动白平衡（自动白平衡在快门帧之前来不及收敛） */
static esp_err_t awb_lock(int fd, const film_still_awb_t *awb)
{
    for (int c = 0; c < OV3640_AWB_CHANNELS; ++c) {
        ESP_RETURN_ON_ERROR(reg_write(fd, (uint16_t)(OV3640_REG_AWB_GAIN_R + c), awb->gain[c]), TAG, "awb gain %d", c);
    }
    uint8_t ctrl = 0;
    ESP_RETURN_ON_ERROR(reg_read(fd, OV3640_REG_AWB_CTRL, &ctrl), TAG, "awb ctrl");
    return reg_write(fd, OV3640_REG_AWB_CTRL, (uint8_t)(ctrl | OV3640_AWB_MANUAL_BIT));
}

/* ---------------------------------------------------------------- 原生流 */

static esp_err_t find_jpeg_format(int fd, esp_cam_sensor_format_t *ret)
{
    for (uint32_t i = 0;; ++i) {
        struct v4l2_sensor_format_enum entry = { .index = i };
        if (ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &entry) != 0) {
            return ESP_ERR_NOT_FOUND;
        }
        const esp_cam_sensor_format_t *f = &entry.format;
        if (f->format == ESP_CAM_SENSOR_PIXFORMAT_JPEG && f->width == FILM_STILL_WIDTH &&
            f->height == FILM_STILL_HEIGHT) {
            *ret = *f;
            return ESP_OK;
        }
    }
}

static esp_err_t still_open(still_t *s)
{
    s->fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    ESP_RETURN_ON_FALSE(s->fd >= 0, ESP_ERR_NOT_FOUND, TAG, "open %s errno=%d", ESP_VIDEO_DVP_DEVICE_NAME, errno);
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_G_SENSOR_FMT, &s->preview_format, "G_SENSOR_FMT"), TAG, "preview fmt");
    ESP_RETURN_ON_ERROR(exposure_read(s->fd, &s->preview_exposure), TAG, "preview exposure");
    esp_cam_sensor_format_t capture;
    ESP_RETURN_ON_ERROR(find_jpeg_format(s->fd, &capture), TAG, "JPEG %dx%d format", FILM_STILL_WIDTH,
                        FILM_STILL_HEIGHT);
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_S_SENSOR_FMT, &capture, "S_SENSOR_FMT capture"), TAG, "switch");
    s->switched = true;
    exposure_t reset;
    ESP_RETURN_ON_ERROR(exposure_read(s->fd, &reset), TAG, "capture exposure");
    ESP_RETURN_ON_ERROR(exposure_transfer(s->fd, &s->preview_exposure, &reset, true, STILL_EXPOSURE_BOOST), TAG,
                        "carry exposure");
    ESP_LOGD(TAG, "still exposure: preview gain %02x %02x lines %u hts %u vts %u | capture hts %u vts %u boost %.2f",
             s->preview_exposure.gain_h, s->preview_exposure.gain_l, s->preview_exposure.exposure,
             s->preview_exposure.hts, s->preview_exposure.vts, reset.hts, reset.vts, (double)STILL_EXPOSURE_BOOST);
    ESP_RETURN_ON_ERROR(replay_awb_tuning(s->fd), TAG, "awb tuning");
    taskENTER_CRITICAL(&s_awb_lock);
    s->awb = s_awb;
    taskEXIT_CRITICAL(&s_awb_lock);
    ESP_RETURN_ON_ERROR(awb_lock(s->fd, &s->awb), TAG, "lock awb");

    struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_G_FMT, &format, "G_FMT"), TAG, "get format");
    format.fmt.pix.width = FILM_STILL_WIDTH;
    format.fmt.pix.height = FILM_STILL_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    /* 不设置时 esp_video 按 宽×高×2 申请（每块 6 MB） */
    format.fmt.pix.sizeimage = STILL_BUFFER_BYTES;
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_S_FMT, &format, "S_FMT"), TAG, "set JPEG");
    struct timeval timeout = { .tv_sec = STILL_FRAME_TIMEOUT_MS / 1000 };
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout, "S_DQBUF_TIMEOUT"), TAG, "timeout");

    struct v4l2_requestbuffers request = {
        .count = STILL_BUFFERS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_REQBUFS, &request, "REQBUFS"), TAG, "buffers");
    s->count = request.count < STILL_BUFFERS ? request.count : STILL_BUFFERS;
    for (uint32_t i = 0; i < s->count; ++i) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = i };
        ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_QUERYBUF, &b, "QUERYBUF"), TAG, "query");
        void *data = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, b.m.offset);
        ESP_RETURN_ON_FALSE(data != MAP_FAILED, ESP_ERR_NO_MEM, TAG, "mmap");
        s->buffers[i] = data;
        s->lengths[i] = b.length;
        ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_QBUF, &b, "QBUF"), TAG, "queue");
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_STREAMON, &type, "STREAMON"), TAG, "stream on");
    s->streaming = true;
    return ESP_OK;
}

/* 停流、释放缓冲、恢复取景格式与曝光；对未完全打开的流也可调用 */
static esp_err_t still_close(still_t *s)
{
    if (s->fd < 0) {
        return ESP_OK;
    }
    esp_err_t err = ESP_OK;
    if (s->streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        err = v4l2(s->fd, VIDIOC_STREAMOFF, &type, "STREAMOFF");
    }
    for (uint32_t i = 0; i < STILL_BUFFERS; ++i) {
        if (s->buffers[i]) {
            (void)munmap(s->buffers[i], s->lengths[i]);
        }
    }
    if (s->count) {
        struct v4l2_requestbuffers request = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        const esp_err_t r = v4l2(s->fd, VIDIOC_REQBUFS, &request, "REQBUFS 0");
        err = err != ESP_OK ? err : r;
    }
    if (s->switched) {
        esp_err_t r = v4l2(s->fd, VIDIOC_S_SENSOR_FMT, &s->preview_format, "S_SENSOR_FMT preview");
        exposure_t reset;
        if (r == ESP_OK) {
            r = exposure_read(s->fd, &reset);
        }
        if (r == ESP_OK) {
            r = exposure_transfer(s->fd, &s->preview_exposure, &reset, false, 1.0f);
        }
        if (r == ESP_OK) {
            r = replay_awb_tuning(s->fd);   /* 取景恢复自动白平衡 */
        }
        err = err != ESP_OK ? err : r;
    }
    close(s->fd);
    s->fd = -1;
    return err;
}

/*
 * DVP 偶尔丢字节或截断，坏帧的头部段解析不过（"JPEG segment format error"），
 * 存成原片后冲洗才失败、这张就白拍了。取帧时就检查 SOI/EOI 并试解析头部，坏帧直接跳过。
 */
static bool frame_valid(const uint8_t *jpeg, size_t size)
{
    if (size <= JPEG_EOI_SEARCH_BYTES || size > STILL_BUFFER_BYTES || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }
    bool eoi = false;
    for (size_t i = size - 2; i >= size - JPEG_EOI_SEARCH_BYTES && !eoi; --i) {
        eoi = jpeg[i] == 0xFF && jpeg[i + 1] == 0xD9;
    }
    if (!eoi) {
        return false;
    }
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = NULL;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) {
        return true;   /* 校验器开不起来就不拦，交给冲洗阶段 */
    }
    jpeg_dec_io_t io = { .inbuf = (uint8_t *)jpeg, .inbuf_len = (int)size };
    jpeg_dec_header_info_t info = { 0 };
    const bool ok = jpeg_dec_parse_header(decoder, &io, &info) == JPEG_ERR_OK && info.width == FILM_STILL_WIDTH &&
                    info.height == FILM_STILL_HEIGHT;
    jpeg_dec_close(decoder);
    return ok;
}

/* 把第 STILL_TAKE_VALID_FRAME 张有效 JPEG 复制进 copy（容量 STILL_BUFFER_BYTES） */
static esp_err_t grab(still_t *s, uint8_t *copy, size_t *ret_size)
{
    int valid = 0;
    for (int i = 0; i < STILL_MAX_FRAMES; ++i) {
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP };
        ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_DQBUF, &b, "DQBUF"), TAG, "dequeue");
        ESP_RETURN_ON_FALSE(b.index < s->count, ESP_FAIL, TAG, "bad buffer");
        const uint8_t *jpeg = s->buffers[b.index];
        const bool ok = frame_valid(jpeg, b.bytesused);
        if (!ok) {
            ESP_LOGW(TAG, "frame %d rejected (%u bytes)", i, (unsigned)b.bytesused);
        }
        if (ok && ++valid == STILL_TAKE_VALID_FRAME) {
            memcpy(copy, jpeg, b.bytesused);
            *ret_size = b.bytesused;
            (void)v4l2(s->fd, VIDIOC_QBUF, &b, "QBUF");
            return ESP_OK;
        }
        ESP_RETURN_ON_ERROR(v4l2(s->fd, VIDIOC_QBUF, &b, "QBUF"), TAG, "requeue");
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t film_still_capture(film_still_t *ret)
{
    const int64_t t0 = esp_timer_get_time();
    *ret = (film_still_t) { 0 };
    /* 开流前先拿到副本缓冲：采集缓冲会占掉大块连续 PSRAM */
    uint8_t *copy = heap_caps_malloc(STILL_BUFFER_BYTES, STILL_COPY_CAPS);
    ESP_RETURN_ON_FALSE(copy, ESP_ERR_NO_MEM, TAG, "still copy buffer");
    still_t s = { .fd = -1 };
    esp_err_t err = still_open(&s);
    if (err == ESP_OK) {
        err = grab(&s, copy, &ret->size);
    }
    const esp_err_t close_err = still_close(&s);
    if (err == ESP_OK && close_err != ESP_OK) {
        ESP_LOGW(TAG, "restore preview format: %s", esp_err_to_name(close_err));
    }
    if (err == ESP_OK) {
        uint8_t *shrunk = heap_caps_realloc(copy, ret->size, STILL_COPY_CAPS);
        ret->jpeg = shrunk ? shrunk : copy;
        ret->awb = s.awb;
    } else {
        heap_caps_free(copy);
    }
    ESP_LOGI(TAG, "still %s: %u bytes in %lld ms, awb %02x %02x %02x", esp_err_to_name(err),
             err == ESP_OK ? (unsigned)ret->size : 0, (long long)((esp_timer_get_time() - t0) / 1000), s.awb.gain[0],
             s.awb.gain[1], s.awb.gain[2]);
    return err;
}

static uint8_t scale_gain(uint8_t gain, float factor)
{
    const long v = lroundf((float)gain * factor);
    return (uint8_t)(v < OV3640_AWB_MIN ? OV3640_AWB_MIN : (v > OV3640_AWB_MAX ? OV3640_AWB_MAX : v));
}

void film_still_learn_awb(const film_still_awb_t *used, film_balance_t residual)
{
    if (!used || residual == FILM_BALANCE_NONE) {
        return;   /* 没有残差（或小样统计无效）：保持现有增益 */
    }
    float gain[3];
    film_balance_gains(residual, gain);
    const film_still_awb_t next = { .gain = {
        scale_gain(used->gain[0], powf(gain[0] / gain[1], STILL_AWB_LEARN_RATE)),
        used->gain[1],
        scale_gain(used->gain[2], powf(gain[2] / gain[1], STILL_AWB_LEARN_RATE)),
    } };
    taskENTER_CRITICAL(&s_awb_lock);
    s_awb = next;
    taskEXIT_CRITICAL(&s_awb_lock);
    ESP_LOGI(TAG, "awb learned %02x %02x %02x -> %02x %02x %02x", used->gain[0], used->gain[1], used->gain[2],
             next.gain[0], next.gain[1], next.gain[2]);
}

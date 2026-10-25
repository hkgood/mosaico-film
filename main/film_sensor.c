#include "film_sensor.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "linux/videodev2.h"

static const char *TAG = "film_sensor";

static film_capture_pixel_t pixel_of(esp_cam_sensor_output_format_t format)
{
    switch (format) {
    case ESP_CAM_SENSOR_PIXFORMAT_YUV422_UYVY:
        return FILM_CAPTURE_PIXEL_UYVY;
    case ESP_CAM_SENSOR_PIXFORMAT_JPEG:
        return FILM_CAPTURE_PIXEL_JPEG;
    default:
        return FILM_CAPTURE_PIXEL_OTHER;
    }
}

static int open_device(void)
{
    const int fd = open(ESP_VIDEO_DVP_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s: errno=%d", ESP_VIDEO_DVP_DEVICE_NAME, errno);
    }
    return fd;
}

esp_err_t film_sensor_list_modes(film_capture_mode_t *modes, size_t capacity, size_t *ret_count)
{
    ESP_RETURN_ON_FALSE(modes && ret_count, ESP_ERR_INVALID_ARG, TAG, "bad args");
    *ret_count = 0;
    const int fd = open_device();
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_ERR_NOT_FOUND, TAG, "no video device");
    for (uint32_t i = 0; i < capacity; ++i) {
        struct v4l2_sensor_format_enum entry = { .index = i };
        if (ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &entry) != 0) {
            break;   /* 枚举到头 */
        }
        const esp_cam_sensor_format_t *f = &entry.format;
        modes[i] = (film_capture_mode_t){
            .pixel = pixel_of(f->format), .width = f->width, .height = f->height, .fps = f->fps,
        };
        ESP_LOGI(TAG, "mode %u: %s", (unsigned)i, f->name ? f->name : "?");
        *ret_count = i + 1;
    }
    close(fd);
    return *ret_count ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t film_sensor_apply_mode(size_t index)
{
    const int fd = open_device();
    ESP_RETURN_ON_FALSE(fd >= 0, ESP_ERR_NOT_FOUND, TAG, "no video device");
    struct v4l2_sensor_format_enum entry = { .index = (uint32_t)index };
    esp_err_t err = ESP_OK;
    if (ioctl(fd, VIDIOC_ENUM_SENSOR_FMT, &entry) != 0) {
        ESP_LOGE(TAG, "mode %u not found: errno=%d", (unsigned)index, errno);
        err = ESP_ERR_NOT_FOUND;
    } else if (ioctl(fd, VIDIOC_S_SENSOR_FMT, &entry.format) != 0) {
        ESP_LOGE(TAG, "switch to %s: errno=%d", entry.format.name ? entry.format.name : "?", errno);
        err = ESP_FAIL;
    }
    close(fd);
    return err;
}

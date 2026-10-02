/*
 * NAND 挂载方式与 Vibe Mode（esp-mosaico-recovery 的 factory_nand_update.c）保持一致：
 * 同一条 SPI 总线、SIO 模式、spi_nand_flash 的 FTL 块设备、LittleFS、不格式化。
 * 文件系统由 Vibe Mode 创建；两边看到的是同一个 /nand。
 */
#include "film_storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp/esp_mosaico.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_blockdev.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "spi_nand_flash.h"

static const char *TAG = "film_storage";

#define NAND_MAX_TRANSFER_BYTES 4096
#define WARM_UP_PATH            FILM_STORAGE_PHOTO_DIR "/.warm"
#define WARM_UP_BYTES           4096

/* 单例状态：只在 film_storage_mount 里写（启动阶段由一个任务调用），之后只读 */
static struct {
    bool mounted;
    bool bus_initialized;
    spi_device_handle_t spi;
    esp_blockdev_handle_t blockdev;
} s_nand;

static esp_err_t release_control_pin(gpio_num_t pin)
{
    const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    /* HOLD/WP 低有效：先置高再切输出，避免一瞬间的写保护/暂停 */
    ESP_RETURN_ON_ERROR(gpio_set_level(pin, 1), TAG, "preload GPIO%d", pin);
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "configure GPIO%d", pin);
    return gpio_set_level(pin, 1);
}

static void cleanup_partial(void)
{
    if (s_nand.blockdev && !s_nand.mounted) {
        (void)s_nand.blockdev->ops->release(s_nand.blockdev);
        s_nand.blockdev = NULL;
    }
    if (s_nand.spi) {
        (void)spi_bus_remove_device(s_nand.spi);
        s_nand.spi = NULL;
    }
    if (s_nand.bus_initialized) {
        (void)spi_bus_free(BSP_NAND_SPI_HOST);
        s_nand.bus_initialized = false;
    }
}

static esp_err_t make_dir(const char *path)
{
    if (mkdir(path, 0775) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "mkdir %s failed: errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t film_storage_mount(void)
{
    if (s_nand.mounted) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(release_control_pin(BSP_NAND_HOLD), TAG, "NAND HOLD");
    ESP_RETURN_ON_ERROR(release_control_pin(BSP_NAND_WP), TAG, "NAND WP");

    const spi_bus_config_t bus = {
        .mosi_io_num = BSP_NAND_D,
        .miso_io_num = BSP_NAND_Q,
        .sclk_io_num = BSP_NAND_CLK,
        .quadhd_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .max_transfer_sz = NAND_MAX_TRANSFER_BYTES,
    };
    esp_err_t err = spi_bus_initialize(BSP_NAND_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    ESP_RETURN_ON_ERROR(err, TAG, "NAND SPI bus");
    s_nand.bus_initialized = true;

    const spi_device_interface_config_t device = {
        .clock_speed_hz = BSP_NAND_FLASH_DEFAULT_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = BSP_NAND_CS,
        .queue_size = BSP_NAND_FLASH_DEFAULT_QUEUE_SIZE,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    err = spi_bus_add_device(BSP_NAND_SPI_HOST, &device, &s_nand.spi);
    if (err == ESP_OK) {
        spi_nand_flash_config_t nand = {
            .device_handle = s_nand.spi,
            .gc_factor = 0,
            .io_mode = SPI_NAND_IO_MODE_SIO,
            .flags = SPI_DEVICE_HALFDUPLEX,
        };
        err = spi_nand_flash_init_with_layers(&nand, &s_nand.blockdev);
    }
    if (err == ESP_OK) {
        const esp_vfs_littlefs_conf_t mount = {
            .base_path = FILM_STORAGE_MOUNT,
            .blockdev = s_nand.blockdev,
            .format_if_mount_failed = false,   /* 绝不格式化：NAND 上还有系统更新包 */
            .read_only = false,
            .dont_mount = false,
            .grow_on_mount = false,
        };
        err = esp_vfs_littlefs_register(&mount);
        if (err != ESP_OK) {
            s_nand.blockdev = NULL;   /* LittleFS 挂载失败时已释放块设备 */
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NAND mount failed: %s", esp_err_to_name(err));
        cleanup_partial();
        return err;
    }
    s_nand.mounted = true;

    err = make_dir(FILM_STORAGE_MOUNT "/DCIM");
    if (err == ESP_OK) {
        err = make_dir(FILM_STORAGE_PHOTO_DIR);
    }
    size_t total = 0, used = 0;
    (void)esp_littlefs_blockdev_info(s_nand.blockdev, &total, &used);
    ESP_LOGI(TAG, "NAND mounted at %s, used %u / %u KB", FILM_STORAGE_MOUNT, (unsigned)(used / 1024),
             (unsigned)(total / 1024));
    return err;
}

const char *film_storage_photo_dir(void)
{
    return s_nand.mounted ? FILM_STORAGE_PHOTO_DIR : NULL;
}

void film_storage_warm_up(void)
{
    if (!s_nand.mounted) {
        return;
    }
    /* 比内联上限大，才会真正分配数据块 */
    static const uint8_t pad[WARM_UP_BYTES];
    const int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(WARM_UP_PATH, "wb");
    bool ok = f && fwrite(pad, 1, sizeof(pad), f) == sizeof(pad);
    if (f) {
        ok = fclose(f) == 0 && ok;
    }
    remove(WARM_UP_PATH);
    ESP_LOGI(TAG, "allocator warm-up %s in %lld ms", ok ? "done" : "failed",
             (long long)((esp_timer_get_time() - t0) / 1000));
}

uint64_t film_storage_free_bytes(void)
{
    size_t total = 0, used = 0;
    if (!s_nand.mounted || esp_littlefs_blockdev_info(s_nand.blockdev, &total, &used) != ESP_OK) {
        return 0;
    }
    return total > used ? (uint64_t)(total - used) : 0;
}

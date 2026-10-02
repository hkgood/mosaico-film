/*
 * 主机端命令行：用设备同一份滤镜代码冲洗一张 PPM 图，供对比脚本和人工看效果。
 *   film_cli <输入.ppm> <输出.ppm> <胶卷键名> [--seed N] [--leak] [--no-grain] [--repeat N] [--parallel]
 * --repeat 用于粗测主机上的耗时（墙钟时间，含每次复制原图）；--parallel 开启双线程冲洗。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "film_filter.h"
#include "ppm_io.h"

static int usage(void)
{
    fprintf(stderr, "usage: film_cli <in.ppm> <out.ppm> <film> [--seed N] [--leak] [--no-grain] [--repeat N]"
            " [--parallel]\n");
    return 2;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        return usage();
    }
    film_develop_params_t params = { .seed = 1, .grain = true };
    if (film_filter_find_by_key(argv[3], &params.film) != ESP_OK) {
        fprintf(stderr, "unknown film: %s\n", argv[3]);
        return 2;
    }
    int repeat = 1;
    bool parallel = false;
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            params.seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--leak") == 0) {
            params.light_leak = true;
        } else if (strcmp(argv[i], "--no-grain") == 0) {
            params.grain = false;
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            repeat = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--parallel") == 0) {
            parallel = true;
        } else {
            return usage();
        }
    }

    ppm_image_t src;
    if (!ppm_read(argv[1], &src)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    size_t size = (size_t)src.width * src.height * 3;
    uint8_t *work = malloc(size);
    film_filter_handle_t filter = NULL;
    film_filter_config_t config = {
        .max_width = (uint16_t)src.width, .max_height = (uint16_t)src.height, .parallel = parallel,
    };
    if (work == NULL || film_filter_create(&config, &filter) != ESP_OK) {
        fprintf(stderr, "out of memory\n");
        free(work);
        free(src.pixels);
        return 1;
    }

    film_image_t image = {
        .pixels = work, .width = (uint16_t)src.width, .height = (uint16_t)src.height,
        .stride = (size_t)src.width * 3,
    };
    esp_err_t err = ESP_OK;
    double start = now_ms();
    for (int i = 0; i < repeat && err == ESP_OK; i++) {
        memcpy(work, src.pixels, size);
        err = film_filter_develop(filter, &params, &image);
    }
    double ms = (now_ms() - start) / repeat;

    int status = 0;
    if (err != ESP_OK) {
        fprintf(stderr, "develop failed: 0x%x\n", err);
        status = 1;
    } else {
        ppm_image_t out = { .pixels = work, .width = src.width, .height = src.height };
        if (!ppm_write(argv[2], &out)) {
            fprintf(stderr, "cannot write %s\n", argv[2]);
            status = 1;
        } else if (repeat > 1) {
            printf("%s %dx%d: %.2f ms/frame\n", argv[3], src.width, src.height, ms);
        }
    }
    film_filter_delete(filter);
    free(work);
    free(src.pixels);
    return status;
}

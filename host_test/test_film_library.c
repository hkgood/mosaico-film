/*
 * 照片库批量删除：跳过不在库里的与重复的编号、只删选中的、文件一起删掉、重开后索引一致。
 *
 *   film_library_test（在 /tmp 下建临时照片目录）
 */
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "film_library.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define PHOTOS 5

static void touch_files(const char *root, uint32_t id)
{
    char path[512];
    for (int kind = FILM_FILE_JPEG; kind <= FILM_FILE_THUMB; ++kind) {
        film_library_path(root, id, (film_file_kind_t)kind, path, sizeof(path));
        FILE *f = fopen(path, "wb");
        CHECK(f);
        fputc(0, f);
        fclose(f);
    }
}

static bool has_file(const char *root, uint32_t id, film_file_kind_t kind)
{
    char path[512];
    film_library_path(root, id, kind, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

int main(void)
{
    /* 照片库根目录有长度上限（FILM_PATH_MAX），构建目录太深，放到 /tmp 下 */
    char root[] = "/tmp/film_lib_XXXXXX";
    CHECK(mkdtemp(root));

    film_library_handle_t lib;
    const film_library_config_t cfg = { .root = root };
    CHECK(film_library_create(&cfg, &lib) == ESP_OK);
    for (int i = 0; i < PHOTOS; ++i) {
        const film_photo_t p = {
            .id = film_library_reserve_id(lib), .screen_w = 480, .screen_h = 360, .thumb_w = 158, .thumb_h = 119,
        };
        touch_files(root, p.id);
        CHECK(film_library_add(lib, &p) == ESP_OK);
    }
    CHECK(film_library_count(lib) == PHOTOS);

    /* 参数错误与空操作 */
    size_t removed = 99;
    CHECK(film_library_remove_many(NULL, NULL, 0, &removed) == ESP_ERR_INVALID_ARG && removed == 0);
    CHECK(film_library_remove_many(lib, NULL, 2, NULL) == ESP_ERR_INVALID_ARG);
    CHECK(film_library_remove_many(lib, NULL, 0, &removed) == ESP_ERR_NOT_FOUND && removed == 0);
    const uint32_t missing[] = { 77, 78 };
    CHECK(film_library_remove_many(lib, missing, 2, &removed) == ESP_ERR_NOT_FOUND && removed == 0);
    CHECK(film_library_count(lib) == PHOTOS);

    /* 删 2、4（4 重复一次，77 不存在）：实际删 2 张，文件一起没了，其他照片不动 */
    const uint32_t ids[] = { 2, 4, 77, 4 };
    CHECK(film_library_remove_many(lib, ids, 4, &removed) == ESP_OK && removed == 2);
    CHECK(film_library_count(lib) == PHOTOS - 2);
    CHECK(!film_library_find(lib, 2, NULL) && !film_library_find(lib, 4, NULL));
    CHECK(film_library_find(lib, 1, NULL) && film_library_find(lib, 3, NULL) && film_library_find(lib, 5, NULL));
    for (int kind = FILM_FILE_JPEG; kind <= FILM_FILE_THUMB; ++kind) {
        CHECK(!has_file(root, 2, (film_file_kind_t)kind) && !has_file(root, 4, (film_file_kind_t)kind));
        CHECK(has_file(root, 3, (film_file_kind_t)kind));
    }
    /* 最新在前的顺序保持不变 */
    CHECK(film_library_get(lib, 0)->id == 5 && film_library_get(lib, 1)->id == 3 && film_library_get(lib, 2)->id == 1);
    film_library_delete(lib);

    /* 重开：索引只写了一次，内容与内存一致 */
    CHECK(film_library_create(&cfg, &lib) == ESP_OK);
    CHECK(film_library_count(lib) == PHOTOS - 2);
    CHECK(film_library_get(lib, 0)->id == 5 && film_library_get(lib, 2)->id == 1);

    /* 全部删光 */
    const uint32_t rest[] = { 1, 3, 5 };
    CHECK(film_library_remove_many(lib, rest, 3, &removed) == ESP_OK && removed == 3);
    CHECK(film_library_count(lib) == 0);
    film_library_delete(lib);
    CHECK(film_library_create(&cfg, &lib) == ESP_OK);
    CHECK(film_library_count(lib) == 0);
    film_library_delete(lib);

    printf("film_library_test: OK\n");
    return 0;
}

/*
 * 待冲洗标记与断电续冲恢复：编码往返、损坏检测、恢复扫描（孤儿 RAW、截断 RAW、已冲完的残留标记）、
 * 相册编号避开待冲洗照片。
 *
 *   film_pending_test（在 /tmp 下建临时照片目录）
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "film_library.h"
#include "film_pending.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static bool has_file(const char *root, uint32_t id, film_file_kind_t kind)
{
    char path[512];
    film_library_path(root, id, kind, path, sizeof(path));
    struct stat st;
    return stat(path, &st) == 0;
}

static void write_file(const char *root, uint32_t id, film_file_kind_t kind, const void *data, size_t size)
{
    char path[512];
    film_library_path(root, id, kind, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    CHECK(f);
    CHECK(fwrite(data, 1, size, f) == size);
    fclose(f);
}

/** 完整的"JPEG"：SOI + 内容 + EOI */
static void write_raw(const char *root, uint32_t id, bool complete)
{
    uint8_t jpeg[64];
    memset(jpeg, 0x11, sizeof(jpeg));
    jpeg[0] = 0xFF;
    jpeg[1] = 0xD8;
    jpeg[sizeof(jpeg) - 2] = complete ? 0xFF : 0x00;
    jpeg[sizeof(jpeg) - 1] = complete ? 0xD9 : 0x00;
    write_file(root, id, FILM_FILE_RAW, jpeg, sizeof(jpeg));
}

static film_shot_t sample_shot(uint32_t id)
{
    return (film_shot_t){
        .photo_id = id, .film = FILM_ID_NIGHT, .exposure_ev = -1.333f, .instant = true, .crop_43 = true,
        .date_stamp = true, .light_leak = true, .rot = GFX_ROT_270, .seed = 0xDEADBEEFu, .roll = 513,
        .frame = 17, .time = 1767225600LL + 12345,
    };
}

static void write_marker(const char *root, const film_shot_t *shot)
{
    uint8_t buf[FILM_PENDING_BYTES];
    film_pending_encode(shot, buf);
    write_file(root, shot->photo_id, FILM_FILE_PENDING, buf, sizeof(buf));
}

static void test_codec(void)
{
    const film_shot_t in = sample_shot(42);
    uint8_t buf[FILM_PENDING_BYTES];
    film_pending_encode(&in, buf);
    film_shot_t out = { 0 };
    CHECK(film_pending_decode(buf, sizeof(buf), &out));
    CHECK(out.photo_id == 42 && out.film == FILM_ID_NIGHT && out.instant && out.crop_43 && out.date_stamp &&
          out.light_leak && out.rot == GFX_ROT_270 && out.seed == 0xDEADBEEFu && out.roll == 513 &&
          out.frame == 17 && out.time == in.time);
    CHECK(out.exposure_ev > -1.3335f && out.exposure_ev < -1.3325f);
    CHECK(out.source_id == 0 && !out.preview_only && out.balance == 0);

    /* 任何一个字节翻转都必须被发现 */
    for (size_t i = 0; i < sizeof(buf); ++i) {
        uint8_t bad[FILM_PENDING_BYTES];
        memcpy(bad, buf, sizeof(bad));
        bad[i] ^= 0x40;
        CHECK(!film_pending_decode(bad, sizeof(bad), &out));
    }
    CHECK(!film_pending_decode(buf, sizeof(buf) - 1, &out));
    uint8_t longer[FILM_PENDING_BYTES + 1] = { 0 };
    memcpy(longer, buf, sizeof(buf));
    CHECK(!film_pending_decode(longer, sizeof(longer), &out));

    /* 未拍过曝光补偿、没有时间的默认值也要往返 */
    film_shot_t zero = { .photo_id = 1, .film = FILM_ID_GOLD };
    film_pending_encode(&zero, buf);
    CHECK(film_pending_decode(buf, sizeof(buf), &out) && out.exposure_ev == 0.0f && out.time == 0);
}

static void test_recover(const char *root)
{
    /* 9：冲洗完成，删 .PND 之前断电（有 .SCR + 残留 .PND）；照片文件不能动 */
    write_raw(root, 9, true);
    write_file(root, 9, FILM_FILE_JPEG, "x", 1);
    write_file(root, 9, FILM_FILE_SCREEN, "x", 1);
    film_shot_t s9 = sample_shot(9);
    write_marker(root, &s9);
    /* 10：普通旧照片，没有 .PND */
    write_raw(root, 10, true);
    write_file(root, 10, FILM_FILE_SCREEN, "x", 1);
    /* 11：待冲洗，RAW 完整 */
    write_raw(root, 11, true);
    film_shot_t s11 = sample_shot(11);
    write_marker(root, &s11);
    /* 12：标记有效但 RAW 被截断 */
    write_raw(root, 12, false);
    film_shot_t s12 = sample_shot(12);
    write_marker(root, &s12);
    /* 13：RAW 写到一半断电，还没有标记（孤儿） */
    write_raw(root, 13, false);
    /* 14：标记损坏 */
    write_raw(root, 14, true);
    write_file(root, 14, FILM_FILE_PENDING, "garbage garbage garbage garbage garbage", 40);
    /* 15：第二张待冲洗 */
    write_raw(root, 15, true);
    film_shot_t s15 = sample_shot(15);
    s15.film = FILM_ID_BW;
    write_marker(root, &s15);

    /* 编号要避开所有这些文件 */
    film_library_handle_t lib;
    const film_library_config_t cfg = { .root = root };
    CHECK(film_library_create(&cfg, &lib) == ESP_OK);
    CHECK(film_library_reserve_id(lib) == 16);
    film_library_delete(lib);
    CHECK(film_library_max_id_on_disk(root) == 15);

    film_shot_t shots[4];
    const size_t n = film_pending_recover(root, shots, 4);
    CHECK(n == 2);
    CHECK(shots[0].photo_id == 11 && shots[0].film == FILM_ID_NIGHT);
    CHECK(shots[1].photo_id == 15 && shots[1].film == FILM_ID_BW);

    CHECK(has_file(root, 9, FILM_FILE_JPEG) && has_file(root, 9, FILM_FILE_SCREEN) && has_file(root, 9, FILM_FILE_RAW));
    CHECK(!has_file(root, 9, FILM_FILE_PENDING));
    CHECK(has_file(root, 10, FILM_FILE_RAW) && has_file(root, 10, FILM_FILE_SCREEN));
    CHECK(has_file(root, 11, FILM_FILE_RAW) && has_file(root, 11, FILM_FILE_PENDING));
    CHECK(!has_file(root, 12, FILM_FILE_RAW) && !has_file(root, 12, FILM_FILE_PENDING));
    CHECK(!has_file(root, 13, FILM_FILE_RAW));
    CHECK(!has_file(root, 14, FILM_FILE_RAW) && !has_file(root, 14, FILM_FILE_PENDING));
    CHECK(has_file(root, 15, FILM_FILE_RAW) && has_file(root, 15, FILM_FILE_PENDING));

    /* 再恢复一次：结果不变（幂等） */
    CHECK(film_pending_recover(root, shots, 4) == 2);
    /* 容量只有 1 张：只返回最早的，另一张留给下次，不能被误删 */
    CHECK(film_pending_recover(root, shots, 1) == 1 && shots[0].photo_id == 11);
    CHECK(has_file(root, 15, FILM_FILE_RAW) && has_file(root, 15, FILM_FILE_PENDING));

    /* 冲洗完成后的善后：删 .PND，RAW 保留 */
    film_pending_remove(root, 11);
    CHECK(!has_file(root, 11, FILM_FILE_PENDING) && has_file(root, 11, FILM_FILE_RAW));
    film_pending_remove(root, 11);   /* 重复删除不出错 */
    CHECK(film_pending_recover(root, shots, 4) == 1 && shots[0].photo_id == 15);
}

int main(void)
{
    char root[] = "/tmp/film_pnd_XXXXXX";
    CHECK(mkdtemp(root));
    test_codec();
    test_recover(root);
    /* 空目录与不存在的目录 */
    film_shot_t shots[2];
    char empty[] = "/tmp/film_pnd_empty_XXXXXX";
    CHECK(mkdtemp(empty));
    CHECK(film_pending_recover(empty, shots, 2) == 0);
    CHECK(film_pending_recover("/tmp/film_pnd_missing_dir", shots, 2) == 0);
    CHECK(film_library_max_id_on_disk("/tmp/film_pnd_missing_dir") == 0);
    printf("film_pending_test: ok\n");
    return 0;
}

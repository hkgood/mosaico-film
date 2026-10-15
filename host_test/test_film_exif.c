/*
 * EXIF 测试：用测试内独立写的 TIFF 解析器核对 film_exif_build 的输出（不借用被测代码的逻辑），
 * 覆盖时间策略、时区、闰年、字符串清洗、容量边界、与 JPEG 的拼接。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "film_exif.h"

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);   \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

/* ---------------------------------------------------------------- 独立的 TIFF 解析 */

typedef struct {
    uint16_t tag, type;
    uint32_t count;
    const uint8_t *value;   /*!< 指向值的字节（内联或数据区），已做边界检查 */
    uint32_t size;
} parsed_entry_t;

typedef struct {
    parsed_entry_t ifd0[16];
    int n0;
    parsed_entry_t exif[16];
    int n1;
} parsed_t;

static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t type_size(uint16_t type)
{
    switch (type) {
    case 1: case 2: case 7: return 1;
    case 3: return 2;
    case 4: return 4;
    case 10: return 8;
    default: return 0;
    }
}

/** 解析一张 IFD：标签必须严格升序、值必须落在 TIFF 范围内；返回条目数，next 为下一 IFD 偏移 */
static int parse_ifd(const uint8_t *tiff, size_t tiff_size, uint32_t offset, parsed_entry_t *out, uint32_t *next)
{
    CHECK(offset + 2 <= tiff_size);
    const int n = u16(tiff + offset);
    CHECK(n >= 1 && n <= 16);
    CHECK(offset + 2 + (size_t)n * 12 + 4 <= tiff_size);
    uint16_t prev_tag = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t *e = tiff + offset + 2 + (size_t)i * 12;
        parsed_entry_t *pe = &out[i];
        pe->tag = u16(e);
        pe->type = u16(e + 2);
        pe->count = u32(e + 4);
        CHECK(pe->tag > prev_tag);                 /* 严格升序（EXIF 规范要求） */
        prev_tag = pe->tag;
        CHECK(type_size(pe->type) != 0);
        pe->size = type_size(pe->type) * pe->count;
        if (pe->size <= 4) {
            pe->value = e + 8;
        } else {
            const uint32_t value_offset = u32(e + 8);
            CHECK(value_offset % 2 == 0);          /* 数据区按字对齐 */
            CHECK((size_t)value_offset + pe->size <= tiff_size);
            pe->value = tiff + value_offset;
        }
    }
    *next = u32(tiff + offset + 2 + (size_t)n * 12);
    return n;
}

static const parsed_entry_t *find(const parsed_entry_t *list, int n, uint16_t tag)
{
    for (int i = 0; i < n; ++i) {
        if (list[i].tag == tag) {
            return &list[i];
        }
    }
    return NULL;
}

/** 校验 APP1 外壳并解析两张 IFD */
static void parse_segment(const uint8_t *seg, size_t size, parsed_t *p)
{
    CHECK(size >= 10 && seg[0] == 0xFF && seg[1] == 0xE1);
    CHECK((size_t)((seg[2] << 8) | seg[3]) == size - 2);   /* 长度字段 = 总长 - 标记 2 字节 */
    CHECK(memcmp(seg + 4, "Exif\0\0", 6) == 0);
    const uint8_t *tiff = seg + 10;
    const size_t tiff_size = size - 10;
    CHECK(memcmp(tiff, "II", 2) == 0 && u16(tiff + 2) == 0x002A);
    memset(p, 0, sizeof(*p));
    uint32_t next = 0;
    p->n0 = parse_ifd(tiff, tiff_size, u32(tiff + 4), p->ifd0, &next);
    CHECK(next == 0);
    const parsed_entry_t *ptr = find(p->ifd0, p->n0, 0x8769);
    if (ptr) {
        CHECK(ptr->type == 4 && ptr->count == 1);
        p->n1 = parse_ifd(tiff, tiff_size, u32(ptr->value), p->exif, &next);
        CHECK(next == 0);
    }
}

static void expect_ascii(const parsed_entry_t *e, const char *text)
{
    CHECK(e && e->type == 2);
    CHECK(e->count == strlen(text) + 1);
    CHECK(memcmp(e->value, text, e->count) == 0);   /* 含结尾 NUL */
}

/* ---------------------------------------------------------------- 用例 */

static film_exif_t full_input(void)
{
    return (film_exif_t) {
        .make = "Mosaico", .model = "Film Camera", .software = "mosaico_film 0.4",
        .description = "STRAY 3200 / roll 3 / frame 12",
        .user_comment = "{\"film\":\"STRAY 3200\",\"ev\":0.33}",
        .time = 1735689600LL, .has_utc_offset = true, .utc_offset_min = 480,
        .orientation = 6, .width = 2048, .height = 1536,
        .has_exposure_bias = true, .exposure_bias_ev = -0.33f,
    };
}

static void test_full_fields(void)
{
    uint8_t seg[FILM_EXIF_SEGMENT_MAX];
    const film_exif_t in = full_input();
    const size_t n = film_exif_build(&in, seg, sizeof(seg));
    CHECK(n > 0);
    parsed_t p;
    parse_segment(seg, n, &p);

    expect_ascii(find(p.ifd0, p.n0, 0x010F), "Mosaico");
    expect_ascii(find(p.ifd0, p.n0, 0x0110), "Film Camera");
    expect_ascii(find(p.ifd0, p.n0, 0x0131), "mosaico_film 0.4");
    expect_ascii(find(p.ifd0, p.n0, 0x010E), "STRAY 3200 / roll 3 / frame 12");
    const parsed_entry_t *orient = find(p.ifd0, p.n0, 0x0112);
    CHECK(orient && orient->type == 3 && u16(orient->value) == 6);

    /* 2025-01-01 00:00:00 UTC，东八区当地时间 08:00:00 */
    expect_ascii(find(p.ifd0, p.n0, 0x0132), "2025:01:01 08:00:00");
    expect_ascii(find(p.exif, p.n1, 0x9003), "2025:01:01 08:00:00");
    expect_ascii(find(p.exif, p.n1, 0x9004), "2025:01:01 08:00:00");
    expect_ascii(find(p.exif, p.n1, 0x9010), "+08:00");
    expect_ascii(find(p.exif, p.n1, 0x9011), "+08:00");
    expect_ascii(find(p.exif, p.n1, 0x9012), "+08:00");

    const parsed_entry_t *bias = find(p.exif, p.n1, 0x9204);
    CHECK(bias && bias->type == 10 && (int32_t)u32(bias->value) == -33 && (int32_t)u32(bias->value + 4) == 100);

    /* UserComment：8 字节字符集前缀 + 正文（无 NUL） */
    const parsed_entry_t *uc = find(p.exif, p.n1, 0x9286);
    const char *json = "{\"film\":\"STRAY 3200\",\"ev\":0.33}";
    CHECK(uc && uc->type == 7 && uc->count == 8 + strlen(json));
    CHECK(memcmp(uc->value, "ASCII\0\0\0", 8) == 0 && memcmp(uc->value + 8, json, strlen(json)) == 0);

    const parsed_entry_t *w = find(p.exif, p.n1, 0xA002);
    const parsed_entry_t *h = find(p.exif, p.n1, 0xA003);
    CHECK(w && h && w->type == 4 && u32(w->value) == 2048 && u32(h->value) == 1536);
}

/** 严格时间策略：未校时（0、1970 年附近、2025 年之前）一律不写任何时间标签 */
static void test_time_policy(void)
{
    static const int64_t invalid[] = { 0, 1, 86400, 1000000000LL, FILM_EXIF_TIME_VALID_AFTER - 1, -5 };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        uint8_t seg[FILM_EXIF_SEGMENT_MAX];
        film_exif_t in = full_input();
        in.time = invalid[i];
        const size_t n = film_exif_build(&in, seg, sizeof(seg));
        CHECK(n > 0);                               /* 其他字段照常写 */
        parsed_t p;
        parse_segment(seg, n, &p);
        CHECK(!find(p.ifd0, p.n0, 0x0132));
        for (uint16_t tag = 0x9003; tag <= 0x9012; ++tag) {
            CHECK(!find(p.exif, p.n1, tag));
        }
        CHECK(memmem(seg, n, "1970", 4) == NULL);   /* 不能出现虚假的 1970 年 */
    }

    /* 恰好 2025-01-01 可写 */
    uint8_t seg[FILM_EXIF_SEGMENT_MAX];
    film_exif_t in = { .time = FILM_EXIF_TIME_VALID_AFTER };
    const size_t n = film_exif_build(&in, seg, sizeof(seg));
    CHECK(n > 0);
    parsed_t p;
    parse_segment(seg, n, &p);
    expect_ascii(find(p.ifd0, p.n0, 0x0132), "2025:01:01 00:00:00");
    CHECK(!find(p.exif, p.n1, 0x9010));             /* 偏移未知：不写 OffsetTime */
}

static void check_local_time(int64_t time, int offset_min, const char *expect_time, const char *expect_offset)
{
    uint8_t seg[FILM_EXIF_SEGMENT_MAX];
    const film_exif_t in = { .time = time, .has_utc_offset = true, .utc_offset_min = (int16_t)offset_min };
    const size_t n = film_exif_build(&in, seg, sizeof(seg));
    CHECK(n > 0);
    parsed_t p;
    parse_segment(seg, n, &p);
    expect_ascii(find(p.exif, p.n1, 0x9003), expect_time);
    expect_ascii(find(p.exif, p.n1, 0x9011), expect_offset);
}

/** 日期换算：闰日、世纪非闰年、跨日的负偏移、半小时时区 */
static void test_calendar(void)
{
    check_local_time(1835481599LL, 0, "2028:02:29 23:59:59", "+00:00");          /* 2028 闰日最后一秒 */
    check_local_time(1835481599LL + 1, 0, "2028:03:01 00:00:00", "+00:00");
    check_local_time(4107542400LL, 0, "2100:03:01 00:00:00", "+00:00");          /* 2100 不是闰年 */
    check_local_time(4107542400LL - 1, 0, "2100:02:28 23:59:59", "+00:00");
    check_local_time(1792047428LL, 480, "2026:10:15 14:57:08", "+08:00");
    check_local_time(1735689600LL, -300, "2024:12:31 19:00:00", "-05:00");       /* 负偏移跨回上一年 */
    check_local_time(1735689600LL, 330, "2025:01:01 05:30:00", "+05:30");        /* 印度半小时时区 */
    check_local_time(1735689600LL, -570, "2024:12:31 14:30:00", "-09:30");
}

static void test_sanitize_and_limits(void)
{
    uint8_t seg[FILM_EXIF_SEGMENT_MAX];
    /* 非 ASCII 与控制字符被替换；过长被截断但仍以 NUL 结尾 */
    char long_text[400];
    memset(long_text, 'a', sizeof(long_text) - 1);
    long_text[sizeof(long_text) - 1] = '\0';
    film_exif_t in = { .make = "Mo\xc3\xa9\n", .description = long_text };
    size_t n = film_exif_build(&in, seg, sizeof(seg));
    CHECK(n > 0);
    parsed_t p;
    parse_segment(seg, n, &p);
    expect_ascii(find(p.ifd0, p.n0, 0x010F), "Mo???");
    const parsed_entry_t *d = find(p.ifd0, p.n0, 0x010E);
    CHECK(d && d->count == FILM_EXIF_DESCRIPTION_MAX);
    CHECK(d->value[d->count - 1] == 0 && d->value[d->count - 2] == 'a');

    /* 所有字段取最大值，也要装得进建议的缓冲 */
    char text63[FILM_EXIF_TEXT_MAX + 20], comment[FILM_EXIF_COMMENT_MAX + 50];
    memset(text63, 'm', sizeof(text63) - 1);
    text63[sizeof(text63) - 1] = '\0';
    memset(comment, 'c', sizeof(comment) - 1);
    comment[sizeof(comment) - 1] = '\0';
    in = (film_exif_t) { .make = text63, .model = text63, .software = text63, .description = long_text,
                         .user_comment = comment, .time = 1792047428LL, .has_utc_offset = true,
                         .utc_offset_min = -720, .orientation = 8, .width = 65535, .height = 65535,
                         .has_exposure_bias = true, .exposure_bias_ev = 3.0f };
    n = film_exif_build(&in, seg, sizeof(seg));
    CHECK(n > 0 && n <= sizeof(seg));
    parse_segment(seg, n, &p);
    const parsed_entry_t *uc = find(p.exif, p.n1, 0x9286);
    CHECK(uc && uc->count == 8 + (FILM_EXIF_COMMENT_MAX - 1));

    /* 任何不足的容量都必须返回 0，不能越界写（ASan 构建会抓到越界） */
    for (size_t cap = 0; cap < n; cap += 7) {
        uint8_t *tight = malloc(cap ? cap : 1);
        CHECK(tight);
        CHECK(film_exif_build(&in, tight, cap) == 0);
        free(tight);
    }
    uint8_t *exact = malloc(n);
    CHECK(exact && film_exif_build(&in, exact, n) == n);
    free(exact);

    /* 没有任何字段、曝光补偿异常 */
    CHECK(film_exif_build(&(film_exif_t) { 0 }, seg, sizeof(seg)) == 0);
    CHECK(film_exif_build(NULL, seg, sizeof(seg)) == 0);
    in = (film_exif_t) { .has_exposure_bias = true, .exposure_bias_ev = 1e9f };
    CHECK(film_exif_build(&in, seg, sizeof(seg)) == 0);
    in = (film_exif_t) { .orientation = 9 };   /* 非法取向不写 */
    CHECK(film_exif_build(&in, seg, sizeof(seg)) == 0);
}

/** 一个最小的 JPEG：SOI + JFIF(APP0) + 旧 EXIF(APP1) + 其他 APP1（XMP）+ DQT + 图像数据 + EOI */
static size_t make_jpeg(uint8_t *out, bool with_old_exif)
{
    static const uint8_t jfif[] = { 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0, 1, 1, 0, 0, 1, 0, 1, 0, 0 };
    static const uint8_t old_exif[] = { 0xFF, 0xE1, 0x00, 0x0A, 'E', 'x', 'i', 'f', 0, 0, 0xAA, 0xBB };
    static const uint8_t xmp[] = { 0xFF, 0xE1, 0x00, 0x08, 'h', 't', 't', 'p', 0, 0 };
    static const uint8_t body[] = { 0xFF, 0xDB, 0x00, 0x04, 0x11, 0x22, 0xFF, 0xDA, 0x00, 0x02, 0x33, 0x44, 0xFF, 0xD9 };
    size_t n = 0;
    out[n++] = 0xFF;
    out[n++] = 0xD8;
    memcpy(out + n, jfif, sizeof(jfif));
    n += sizeof(jfif);
    if (with_old_exif) {
        memcpy(out + n, old_exif, sizeof(old_exif));
        n += sizeof(old_exif);
    }
    memcpy(out + n, xmp, sizeof(xmp));
    n += sizeof(xmp);
    memcpy(out + n, body, sizeof(body));
    return n + sizeof(body);
}

static void test_splice(void)
{
    uint8_t seg[FILM_EXIF_SEGMENT_MAX];
    const film_exif_t in = full_input();
    const size_t seg_size = film_exif_build(&in, seg, sizeof(seg));
    CHECK(seg_size > 0);

    for (int old = 0; old <= 1; ++old) {
        uint8_t jpeg[256], out[4096];
        const size_t jn = make_jpeg(jpeg, old != 0);
        const size_t expect = film_exif_spliced_size(jpeg, jn, seg_size);
        CHECK(expect > 0);
        const size_t n = film_exif_splice(jpeg, jn, seg, seg_size, out, sizeof(out));
        CHECK(n == expect);

        /* 结构：SOI、新 EXIF、JFIF、XMP、其余原样；旧 EXIF 不再出现 */
        CHECK(out[0] == 0xFF && out[1] == 0xD8);
        CHECK(memcmp(out + 2, seg, seg_size) == 0);
        const size_t jfif_at = 2 + seg_size;
        CHECK(out[jfif_at] == 0xFF && out[jfif_at + 1] == 0xE0);
        CHECK(memmem(out, n, "\xAA\xBB", 2) == NULL);
        CHECK(memmem(out, n, "http", 4) != NULL);
        CHECK(out[n - 2] == 0xFF && out[n - 1] == 0xD9);
        CHECK(film_exif_read_orientation(out, n) == 6);

        /* 压缩数据部分逐字节不变 */
        static const uint8_t tail[] = { 0xFF, 0xDB, 0x00, 0x04, 0x11, 0x22, 0xFF, 0xDA, 0x00, 0x02, 0x33, 0x44, 0xFF, 0xD9 };
        CHECK(memcmp(out + n - sizeof(tail), tail, sizeof(tail)) == 0);

        /* 再拼一次：只保留最新一个 EXIF，大小不变 */
        uint8_t again[4096];
        CHECK(film_exif_splice(out, n, seg, seg_size, again, sizeof(again)) == n);
        CHECK(memcmp(again, out, n) == 0);

        /* 容量差一个字节也不行 */
        CHECK(film_exif_splice(jpeg, jn, seg, seg_size, out, n - 1) == 0);
    }

    /* 非法输入 */
    uint8_t out[512], not_jpeg[16] = { 0x89, 'P', 'N', 'G' };
    CHECK(film_exif_splice(not_jpeg, sizeof(not_jpeg), seg, seg_size, out, sizeof(out)) == 0);
    CHECK(film_exif_spliced_size(not_jpeg, sizeof(not_jpeg), seg_size) == 0);
    uint8_t jpeg[256];
    const size_t jn = make_jpeg(jpeg, false);
    CHECK(film_exif_splice(jpeg, jn - 12, seg, seg_size, out, sizeof(out)) > 0);   /* 截断在图像数据里仍可拼 */
    jpeg[4] = 0xFF;                                                                 /* 段长度越界 */
    jpeg[5] = 0xFF;
    CHECK(film_exif_splice(jpeg, jn, seg, seg_size, out, sizeof(out)) == 0);
    CHECK(film_exif_read_orientation(NULL, 0) == 0);
}

/** 带 SOF0 的最小 JPEG 头：尺寸应能不解码直接读出，且跨过 APP 段、填充字节后仍正确 */
static void test_jpeg_size(void)
{
    static const uint8_t jpeg[] = {
        0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,          /* APP0（长度 4：只含长度自身 + 2 字节） */
        0xFF, 0xFF, 0xFF, 0xDB, 0x00, 0x04, 0x11, 0x22,          /* 填充字节 + DQT */
        0xFF, 0xC4, 0x00, 0x04, 0x00, 0x00,                      /* DHT（0xC4 不是 SOF，要跳过） */
        0xFF, 0xC0, 0x00, 0x11, 0x08, 0x06, 0x00, 0x08, 0x00,    /* SOF0：高 1536、宽 2048 */
        0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01,
        0xFF, 0xDA, 0x00, 0x02, 0xFF, 0xD9,
    };
    uint16_t w = 0, h = 0;
    CHECK(film_exif_jpeg_size(jpeg, sizeof(jpeg), &w, &h) && w == 2048 && h == 1536);

    /* 拼入 EXIF 之后尺寸不变 */
    uint8_t seg[FILM_EXIF_SEGMENT_MAX], out[512];
    const size_t seg_size = film_exif_build(&(film_exif_t) { .orientation = 3 }, seg, sizeof(seg));
    const size_t n = film_exif_splice(jpeg, sizeof(jpeg), seg, seg_size, out, sizeof(out));
    CHECK(n > 0 && film_exif_jpeg_size(out, n, &w, &h) && w == 2048 && h == 1536);

    CHECK(!film_exif_jpeg_size(jpeg, 20, &w, &h));              /* 在 SOF 之前截断 */
    CHECK(!film_exif_jpeg_size(jpeg, 30, &w, &h));              /* SOF 被截断 */
    CHECK(!film_exif_jpeg_size(NULL, 0, &w, &h));
    static const uint8_t no_sof[] = { 0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x02, 0xFF, 0xD9 };
    CHECK(!film_exif_jpeg_size(no_sof, sizeof(no_sof), &w, &h));
    static const uint8_t zero[] = { 0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0, 0, 0, 0, 0x01, 0x01, 0x11, 0x00 };
    CHECK(!film_exif_jpeg_size(zero, sizeof(zero), &w, &h));    /* 尺寸为 0 视为无效 */
}

int main(void)
{
    test_full_fields();
    test_time_policy();
    test_calendar();
    test_sanitize_and_limits();
    test_splice();
    test_jpeg_size();
    printf("film_exif_test: ok\n");
    return 0;
}

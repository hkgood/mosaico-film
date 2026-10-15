/*
 * EXIF（APP1）段生成与拼接。TIFF 结构：
 *
 *   [0]      "II" 0x002A  IFD0 偏移（=8）
 *   [8]      IFD0（按标签升序）        → 含指向 Exif IFD 的 0x8769
 *   [...]    Exif IFD（按标签升序）
 *   [...]    超过 4 字节的值（数据区，按 2 字节对齐）
 *
 * 做法：先把每个字段整理成 entry_t（标签、类型、个数、字节数据），算好版面，再顺序写出。
 * 所有写入都经过带边界检查的 writer_t，溢出只置位标志，最后统一返回 0。
 */
#include "film_exif.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* TIFF 数据类型 */
#define TYPE_ASCII      2
#define TYPE_SHORT      3
#define TYPE_LONG       4
#define TYPE_UNDEFINED  7
#define TYPE_SRATIONAL  10

/* 标签 */
#define TAG_IMAGE_DESCRIPTION   0x010E
#define TAG_MAKE                0x010F
#define TAG_MODEL               0x0110
#define TAG_ORIENTATION         0x0112
#define TAG_SOFTWARE            0x0131
#define TAG_DATE_TIME           0x0132
#define TAG_EXIF_IFD            0x8769
#define TAG_DATE_TIME_ORIGINAL  0x9003
#define TAG_DATE_TIME_DIGITIZED 0x9004
#define TAG_OFFSET_TIME         0x9010
#define TAG_OFFSET_TIME_ORIGINAL 0x9011
#define TAG_OFFSET_TIME_DIGITIZED 0x9012
#define TAG_EXPOSURE_BIAS       0x9204
#define TAG_USER_COMMENT        0x9286
#define TAG_PIXEL_X_DIMENSION   0xA002
#define TAG_PIXEL_Y_DIMENSION   0xA003

#define IFD_ENTRY_BYTES   12
#define TIFF_HEADER_BYTES 8
#define APP1_PREFIX_BYTES 10          /* FFE1 + 长度(2) + "Exif\0\0" */
#define APP1_MAX_PAYLOAD  65533       /* 长度字段 16 位，且包含自身 2 字节 */
#define MAX_ENTRIES       16
#define SMALL_VALUE_BYTES 8           /* entry 内联存放的短值（SHORT/LONG/SRATIONAL）的最大字节数 */
#define DATE_TIME_BYTES   20          /* "YYYY:MM:DD HH:MM:SS" + NUL */
#define OFFSET_TIME_BYTES 7           /* "+08:00" + NUL */

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    const uint8_t *data;              /*!< 值的字节（小端），大小 = size */
    uint32_t size;
    uint8_t small[SMALL_VALUE_BYTES]; /*!< 短值在这里序列化，data 指向它 */
} entry_t;

typedef struct {
    entry_t items[MAX_ENTRIES];
    size_t count;
} ifd_t;

typedef struct {
    uint8_t *base;
    size_t capacity;
    size_t pos;
    bool overflow;
} writer_t;

/* ---------------------------------------------------------------- 写入工具 */

static void put_bytes(writer_t *w, const void *src, size_t n)
{
    if (w->overflow || w->pos + n > w->capacity) {
        w->overflow = true;
        return;
    }
    memcpy(w->base + w->pos, src, n);
    w->pos += n;
}

static void put_u16(writer_t *w, uint16_t v)
{
    const uint8_t b[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    put_bytes(w, b, sizeof(b));
}

static void put_u32(writer_t *w, uint32_t v)
{
    const uint8_t b[4] = { (uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF), (uint8_t)((v >> 16) & 0xFF),
                           (uint8_t)(v >> 24) };
    put_bytes(w, b, sizeof(b));
}

static void store_u16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)(v >> 8);
}

static void store_u32(uint8_t *dst, uint32_t v)
{
    store_u16(dst, (uint16_t)(v & 0xFFFF));
    store_u16(dst + 2, (uint16_t)(v >> 16));
}

/* ---------------------------------------------------------------- 向 IFD 追加字段 */

static entry_t *ifd_add(ifd_t *ifd, uint16_t tag, uint16_t type, uint32_t count)
{
    if (ifd->count >= MAX_ENTRIES) {
        return NULL;
    }
    entry_t *e = &ifd->items[ifd->count++];
    memset(e, 0, sizeof(*e));
    e->tag = tag;
    e->type = type;
    e->count = count;
    return e;
}

/** 追加 ASCII 值（text 已含结尾 NUL，length 为含 NUL 的字节数；指针须在 build 期间保持有效） */
static void add_ascii(ifd_t *ifd, uint16_t tag, const char *text, uint32_t length)
{
    entry_t *e = ifd_add(ifd, tag, TYPE_ASCII, length);
    if (e) {
        e->data = (const uint8_t *)text;
        e->size = length;
    }
}

static void add_short(ifd_t *ifd, uint16_t tag, uint16_t value)
{
    entry_t *e = ifd_add(ifd, tag, TYPE_SHORT, 1);
    if (e) {
        store_u16(e->small, value);
        e->data = e->small;
        e->size = 2;
    }
}

static void add_long(ifd_t *ifd, uint16_t tag, uint32_t value)
{
    entry_t *e = ifd_add(ifd, tag, TYPE_LONG, 1);
    if (e) {
        store_u32(e->small, value);
        e->data = e->small;
        e->size = 4;
    }
}

static void add_srational(ifd_t *ifd, uint16_t tag, int32_t numerator, int32_t denominator)
{
    entry_t *e = ifd_add(ifd, tag, TYPE_SRATIONAL, 1);
    if (e) {
        store_u32(e->small, (uint32_t)numerator);
        store_u32(e->small + 4, (uint32_t)denominator);
        e->data = e->small;
        e->size = 8;
    }
}

/* ---------------------------------------------------------------- 文本与时间 */

/** 复制为 ASCII：截断到 cap-1 字节、非 ASCII 与控制字符换成 '?'，保证以 NUL 结尾；返回含 NUL 的字节数 */
static uint32_t copy_ascii(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; src[n] && n + 1 < cap; ++n) {
        const unsigned char c = (unsigned char)src[n];
        dst[n] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    dst[n] = '\0';
    return (uint32_t)(n + 1);
}

/** 公历日期（Howard Hinnant 的 civil_from_days）：自 1970-01-01 起的天数 → 年月日 */
static void civil_from_days(int64_t days, int *year, int *month, int *day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const int64_t doe = days - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    *day = (int)(doy - (153 * mp + 2) / 5 + 1);
    *month = (int)(mp < 10 ? mp + 3 : mp - 9);
    *year = (int)(yoe + era * 400 + (*month <= 2 ? 1 : 0));
}

static void format_date_time(char *dst, int64_t local_seconds)
{
    int64_t days = local_seconds / 86400;
    int64_t rem = local_seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    const int hh = (int)(rem / 3600), mm = (int)(rem % 3600 / 60), ss = (int)(rem % 60);
    /* 手写拼接，避免 snprintf 对格式宽度的告警，也不依赖 locale */
    dst[0] = (char)('0' + y / 1000 % 10);
    dst[1] = (char)('0' + y / 100 % 10);
    dst[2] = (char)('0' + y / 10 % 10);
    dst[3] = (char)('0' + y % 10);
    dst[4] = ':';
    dst[5] = (char)('0' + m / 10);
    dst[6] = (char)('0' + m % 10);
    dst[7] = ':';
    dst[8] = (char)('0' + d / 10);
    dst[9] = (char)('0' + d % 10);
    dst[10] = ' ';
    dst[11] = (char)('0' + hh / 10);
    dst[12] = (char)('0' + hh % 10);
    dst[13] = ':';
    dst[14] = (char)('0' + mm / 10);
    dst[15] = (char)('0' + mm % 10);
    dst[16] = ':';
    dst[17] = (char)('0' + ss / 10);
    dst[18] = (char)('0' + ss % 10);
    dst[19] = '\0';
}

static void format_offset(char *dst, int minutes)
{
    const int a = minutes < 0 ? -minutes : minutes;
    dst[0] = minutes < 0 ? '-' : '+';
    dst[1] = (char)('0' + a / 600 % 10);
    dst[2] = (char)('0' + a / 60 % 10);
    dst[3] = ':';
    dst[4] = (char)('0' + a % 60 / 10);
    dst[5] = (char)('0' + a % 10);
    dst[6] = '\0';
}

/* ---------------------------------------------------------------- 版面与写出 */

static size_t ifd_table_bytes(const ifd_t *ifd)
{
    return 2 + IFD_ENTRY_BYTES * ifd->count + 4;
}

/** 超过 4 字节的值要放数据区，按 2 字节对齐 */
static size_t data_bytes(const ifd_t *ifd)
{
    size_t total = 0;
    for (size_t i = 0; i < ifd->count; ++i) {
        if (ifd->items[i].size > 4) {
            total += (ifd->items[i].size + 1u) & ~1u;
        }
    }
    return total;
}

/** 写 IFD 表；大值依次落在 *data_offset 开始的数据区（先登记偏移，数据稍后由 write_data 写出） */
static void write_table(writer_t *w, const ifd_t *ifd, uint32_t *data_offset)
{
    put_u16(w, (uint16_t)ifd->count);
    for (size_t i = 0; i < ifd->count; ++i) {
        const entry_t *e = &ifd->items[i];
        put_u16(w, e->tag);
        put_u16(w, e->type);
        put_u32(w, e->count);
        if (e->size <= 4) {
            uint8_t inline_value[4] = { 0, 0, 0, 0 };
            memcpy(inline_value, e->data, e->size);
            put_bytes(w, inline_value, sizeof(inline_value));
        } else {
            put_u32(w, *data_offset);
            *data_offset += (e->size + 1u) & ~1u;
        }
    }
    put_u32(w, 0);   /* 没有下一个 IFD（不带缩略图） */
}

static void write_data(writer_t *w, const ifd_t *ifd)
{
    static const uint8_t pad = 0;
    for (size_t i = 0; i < ifd->count; ++i) {
        const entry_t *e = &ifd->items[i];
        if (e->size > 4) {
            put_bytes(w, e->data, e->size);
            if (e->size & 1u) {
                put_bytes(w, &pad, 1);
            }
        }
    }
}

size_t film_exif_build(const film_exif_t *exif, uint8_t *out, size_t capacity)
{
    if (!exif || !out) {
        return 0;
    }

    /* 字符串先整理成安全的 ASCII；这些缓冲在整个 build 期间保持有效 */
    char make[FILM_EXIF_TEXT_MAX], model[FILM_EXIF_TEXT_MAX], software[FILM_EXIF_TEXT_MAX];
    char description[FILM_EXIF_DESCRIPTION_MAX];
    char comment[8 + FILM_EXIF_COMMENT_MAX];     /* 前 8 字节是字符集前缀 "ASCII\0\0\0" */
    char date_time[DATE_TIME_BYTES], offset[OFFSET_TIME_BYTES];

    ifd_t ifd0 = { .count = 0 }, exif_ifd = { .count = 0 };

    /* ---- IFD0（标签升序：010E 010F 0110 0112 0131 0132 8769） ---- */
    if (exif->description && exif->description[0]) {
        add_ascii(&ifd0, TAG_IMAGE_DESCRIPTION, description,
                  copy_ascii(description, sizeof(description), exif->description));
    }
    if (exif->make && exif->make[0]) {
        add_ascii(&ifd0, TAG_MAKE, make, copy_ascii(make, sizeof(make), exif->make));
    }
    if (exif->model && exif->model[0]) {
        add_ascii(&ifd0, TAG_MODEL, model, copy_ascii(model, sizeof(model), exif->model));
    }
    if (exif->orientation >= 1 && exif->orientation <= 8) {
        add_short(&ifd0, TAG_ORIENTATION, exif->orientation);
    }
    if (exif->software && exif->software[0]) {
        add_ascii(&ifd0, TAG_SOFTWARE, software, copy_ascii(software, sizeof(software), exif->software));
    }

    /* 时间：只有可信时才写；EXIF 要求当地墙钟时间，已知偏移时换算并写 OffsetTime */
    const bool time_ok = exif->time >= FILM_EXIF_TIME_VALID_AFTER;
    if (time_ok) {
        const int offset_min = exif->has_utc_offset ? exif->utc_offset_min : 0;
        format_date_time(date_time, exif->time + (int64_t)offset_min * 60);
        add_ascii(&ifd0, TAG_DATE_TIME, date_time, DATE_TIME_BYTES);
        if (exif->has_utc_offset) {
            format_offset(offset, offset_min);
        }
    }

    /* ---- Exif IFD（标签升序：9003 9004 9010 9011 9012 9204 9286 A002 A003） ---- */
    if (time_ok) {
        add_ascii(&exif_ifd, TAG_DATE_TIME_ORIGINAL, date_time, DATE_TIME_BYTES);
        add_ascii(&exif_ifd, TAG_DATE_TIME_DIGITIZED, date_time, DATE_TIME_BYTES);
        if (exif->has_utc_offset) {
            add_ascii(&exif_ifd, TAG_OFFSET_TIME, offset, OFFSET_TIME_BYTES);
            add_ascii(&exif_ifd, TAG_OFFSET_TIME_ORIGINAL, offset, OFFSET_TIME_BYTES);
            add_ascii(&exif_ifd, TAG_OFFSET_TIME_DIGITIZED, offset, OFFSET_TIME_BYTES);
        }
    }
    if (exif->has_exposure_bias && isfinite(exif->exposure_bias_ev) && fabsf(exif->exposure_bias_ev) < 100.0f) {
        add_srational(&exif_ifd, TAG_EXPOSURE_BIAS, (int32_t)lroundf(exif->exposure_bias_ev * 100.0f), 100);
    }
    if (exif->user_comment && exif->user_comment[0]) {
        memcpy(comment, "ASCII\0\0\0", 8);
        const uint32_t body = copy_ascii(comment + 8, FILM_EXIF_COMMENT_MAX, exif->user_comment) - 1;  /* 不含 NUL */
        add_ascii(&exif_ifd, TAG_USER_COMMENT, comment, 8 + body);
        exif_ifd.items[exif_ifd.count - 1].type = TYPE_UNDEFINED;
    }
    if (exif->width && exif->height) {
        add_long(&exif_ifd, TAG_PIXEL_X_DIMENSION, exif->width);
        add_long(&exif_ifd, TAG_PIXEL_Y_DIMENSION, exif->height);
    }

    /* Exif IFD 非空才需要 IFD0 里的指针；指针值要等版面算出来，先占位 */
    const bool has_exif_ifd = exif_ifd.count > 0;
    if (has_exif_ifd) {
        add_long(&ifd0, TAG_EXIF_IFD, 0);
    }
    if (ifd0.count == 0 && !has_exif_ifd) {
        return 0;   /* 没有任何字段可写 */
    }

    /* ---- 版面 ---- */
    const uint32_t ifd0_offset = TIFF_HEADER_BYTES;
    const uint32_t exif_offset = (uint32_t)(ifd0_offset + ifd_table_bytes(&ifd0));
    const uint32_t data_start = exif_offset + (has_exif_ifd ? (uint32_t)ifd_table_bytes(&exif_ifd) : 0);
    const size_t tiff_size = (size_t)data_start + data_bytes(&ifd0) + data_bytes(&exif_ifd);
    if (6 + tiff_size + 2 > APP1_MAX_PAYLOAD) {
        return 0;
    }
    if (has_exif_ifd) {
        entry_t *ptr = &ifd0.items[ifd0.count - 1];   /* 0x8769 最大，必在末尾 */
        store_u32(ptr->small, exif_offset);
    }

    /* ---- 写出 ---- */
    writer_t w = { .base = out, .capacity = capacity };
    const uint16_t length = (uint16_t)(2 + 6 + tiff_size);
    const uint8_t marker[4] = { 0xFF, 0xE1, (uint8_t)(length >> 8), (uint8_t)(length & 0xFF) };
    put_bytes(&w, marker, sizeof(marker));
    put_bytes(&w, "Exif\0\0", 6);
    put_bytes(&w, "II", 2);
    put_u16(&w, 0x002A);
    put_u32(&w, ifd0_offset);
    uint32_t data_offset = data_start;
    write_table(&w, &ifd0, &data_offset);
    if (has_exif_ifd) {
        write_table(&w, &exif_ifd, &data_offset);
    }
    write_data(&w, &ifd0);
    write_data(&w, &exif_ifd);
    if (w.overflow || w.pos != APP1_PREFIX_BYTES + tiff_size) {
        return 0;
    }
    return w.pos;
}

/* ---------------------------------------------------------------- JPEG 拼接 */

static bool is_exif_app1(const uint8_t *seg, size_t seg_size)
{
    /* seg 指向标记 0xFF 0xE1；有效载荷以 "Exif\0\0" 开头 */
    return seg_size >= 10 && seg[1] == 0xE1 && memcmp(seg + 4, "Exif\0\0", 6) == 0;
}

/** 读出 SOI 之后连续的 APPn 段范围；返回 false 表示不是合法 JPEG。*ret_end 为第一个非 APPn 段的偏移 */
static bool scan_app_segments(const uint8_t *jpeg, size_t size, size_t *dropped_bytes, size_t *ret_end)
{
    if (!jpeg || size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return false;
    }
    size_t pos = 2, dropped = 0;
    while (pos + 4 <= size && jpeg[pos] == 0xFF && jpeg[pos + 1] >= 0xE0 && jpeg[pos + 1] <= 0xEF) {
        const size_t seg_size = 2u + (((size_t)jpeg[pos + 2] << 8) | jpeg[pos + 3]);
        if (seg_size < 4 || pos + seg_size > size) {
            return false;
        }
        if (is_exif_app1(jpeg + pos, seg_size)) {
            dropped += seg_size;
        }
        pos += seg_size;
    }
    *dropped_bytes = dropped;
    *ret_end = pos;
    return true;
}

size_t film_exif_spliced_size(const uint8_t *jpeg, size_t jpeg_size, size_t segment_size)
{
    size_t dropped = 0, end = 0;
    if (!scan_app_segments(jpeg, jpeg_size, &dropped, &end)) {
        return 0;
    }
    return jpeg_size - dropped + segment_size;
}

size_t film_exif_splice(const uint8_t *jpeg, size_t jpeg_size, const uint8_t *segment, size_t segment_size,
                        uint8_t *out, size_t capacity)
{
    size_t dropped = 0, end = 0;
    if (!segment || segment_size < APP1_PREFIX_BYTES || !out || !scan_app_segments(jpeg, jpeg_size, &dropped, &end)) {
        return 0;
    }
    const size_t total = jpeg_size - dropped + segment_size;
    if (total > capacity) {
        return 0;
    }
    size_t o = 0;
    memcpy(out, jpeg, 2);                      /* SOI */
    o += 2;
    memcpy(out + o, segment, segment_size);    /* 新 EXIF 紧随其后 */
    o += segment_size;
    size_t pos = 2;
    while (pos < end) {                        /* 保留其余 APPn（如 JFIF），跳过旧 EXIF */
        const size_t seg_size = 2u + (((size_t)jpeg[pos + 2] << 8) | jpeg[pos + 3]);
        if (!is_exif_app1(jpeg + pos, seg_size)) {
            memcpy(out + o, jpeg + pos, seg_size);
            o += seg_size;
        }
        pos += seg_size;
    }
    memcpy(out + o, jpeg + end, jpeg_size - end);   /* DQT/SOF/…/图像数据原样 */
    return total;
}

bool film_exif_jpeg_size(const uint8_t *jpeg, size_t jpeg_size, uint16_t *ret_width, uint16_t *ret_height)
{
    if (!jpeg || jpeg_size < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8 || !ret_width || !ret_height) {
        return false;
    }
    size_t pos = 2;
    while (pos + 4 <= jpeg_size) {
        if (jpeg[pos] != 0xFF) {
            return false;                      /* 段之间不应有杂散字节 */
        }
        const uint8_t marker = jpeg[pos + 1];
        if (marker == 0xFF) {                  /* 填充字节 */
            ++pos;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) {
            pos += 2;                          /* 无长度的独立标记 */
            continue;
        }
        if (marker == 0xD9 || marker == 0xDA) {
            return false;                      /* EOI / SOS：之后是压缩数据，不会再有 SOF */
        }
        const size_t seg_size = 2u + (((size_t)jpeg[pos + 2] << 8) | jpeg[pos + 3]);
        const bool is_sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (is_sof) {
            if (seg_size < 9 || pos + 9 > jpeg_size) {
                return false;
            }
            *ret_height = (uint16_t)((jpeg[pos + 5] << 8) | jpeg[pos + 6]);
            *ret_width = (uint16_t)((jpeg[pos + 7] << 8) | jpeg[pos + 8]);
            return *ret_width > 0 && *ret_height > 0;
        }
        pos += seg_size;
    }
    return false;
}

/* ---------------------------------------------------------------- 读取 Orientation（诊断/测试用） */

static uint16_t rd16(const uint8_t *p, bool little)
{
    return little ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t rd32(const uint8_t *p, bool little)
{
    return little ? ((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24))
                  : (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]);
}

uint16_t film_exif_read_orientation(const uint8_t *jpeg, size_t jpeg_size)
{
    size_t dropped = 0, end = 0;
    if (!scan_app_segments(jpeg, jpeg_size, &dropped, &end)) {
        return 0;
    }
    size_t pos = 2;
    while (pos < end) {
        const size_t seg_size = 2u + (((size_t)jpeg[pos + 2] << 8) | jpeg[pos + 3]);
        if (is_exif_app1(jpeg + pos, seg_size)) {
            const uint8_t *tiff = jpeg + pos + 10;
            const size_t tiff_size = seg_size - 10;
            if (tiff_size < TIFF_HEADER_BYTES || (memcmp(tiff, "II", 2) && memcmp(tiff, "MM", 2))) {
                return 0;
            }
            const bool little = tiff[0] == 'I';
            const uint32_t ifd = rd32(tiff + 4, little);
            if ((size_t)ifd + 2 > tiff_size) {
                return 0;
            }
            const uint16_t n = rd16(tiff + ifd, little);
            for (uint16_t i = 0; i < n && (size_t)ifd + 2 + (size_t)(i + 1) * IFD_ENTRY_BYTES <= tiff_size; ++i) {
                const uint8_t *e = tiff + ifd + 2 + (size_t)i * IFD_ENTRY_BYTES;
                if (rd16(e, little) == TAG_ORIENTATION && rd16(e + 2, little) == TYPE_SHORT) {
                    return rd16(e + 8, little);
                }
            }
            return 0;
        }
        pos += seg_size;
    }
    return 0;
}

uint16_t film_exif_orientation_for_raw(int hold_quarter_turns)
{
    /*
     * 原片是传感器方向。暗房先把它逆时针转 90° 得到取景方向，握持方向每加一档再逆时针多转 90°。
     * 所以显示时要顺时针转 (270 - 90 × hold) 度才是正立：
     *   hold 0 → 270°(8)   hold 1 → 180°(3)   hold 2 → 90°(6)   hold 3 → 0°(1)
     * 该映射由 host_test/test_film_darkroom.c 用真实暗房逐方向核对。
     */
    switch (((hold_quarter_turns % 4) + 4) % 4) {
    case 0:
        return FILM_EXIF_ORIENT_ROT270CW;
    case 1:
        return FILM_EXIF_ORIENT_ROT180;
    case 2:
        return FILM_EXIF_ORIENT_ROT90CW;
    default:
        return FILM_EXIF_ORIENT_NORMAL;
    }
}

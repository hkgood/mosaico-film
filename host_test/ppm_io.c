#include "ppm_io.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* 读一个十进制头字段，跳过空白和 # 注释 */
static bool read_header_int(FILE *f, int *ret_value)
{
    int ch = fgetc(f);
    while (ch != EOF && (isspace(ch) || ch == '#')) {
        if (ch == '#') {
            while (ch != EOF && ch != '\n') {
                ch = fgetc(f);
            }
        }
        ch = fgetc(f);
    }
    int value = 0;
    bool any = false;
    while (ch != EOF && isdigit(ch)) {
        value = value * 10 + (ch - '0');
        any = true;
        ch = fgetc(f);
    }
    *ret_value = value;
    return any;
}

bool ppm_read(const char *path, ppm_image_t *ret_image)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    bool ok = false;
    int width = 0, height = 0, maxval = 0;
    if (fgetc(f) == 'P' && fgetc(f) == '6' && read_header_int(f, &width) && read_header_int(f, &height) &&
            read_header_int(f, &maxval) && maxval == 255 && width > 0 && height > 0) {
        size_t size = (size_t)width * height * 3;
        uint8_t *pixels = malloc(size);
        if (pixels != NULL && fread(pixels, 1, size, f) == size) {
            ret_image->pixels = pixels;
            ret_image->width = width;
            ret_image->height = height;
            ok = true;
        } else {
            free(pixels);
        }
    }
    fclose(f);
    return ok;
}

bool ppm_write(const char *path, const ppm_image_t *image)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    size_t size = (size_t)image->width * image->height * 3;
    bool ok = fprintf(f, "P6\n%d %d\n255\n", image->width, image->height) > 0 &&
              fwrite(image->pixels, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

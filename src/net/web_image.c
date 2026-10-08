#include "net/web_image.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/nanosvg.h"
#include "vendor/nanosvgrast.h"
#include "vendor/stb_image.h"

#define MAX_SOURCE_PIXELS (16u * 1024 * 1024)
#define SVG_DPI           96.0f
#define GREY_LEVELS       4
#define MIN_CODE_SIZE     2
#define CLEAR_CODE        (1 << MIN_CODE_SIZE)
#define END_CODE          (CLEAR_CODE + 1)
#define MAX_CODE          4095
#define USE_PASSES        8
#define SVG_MAX_LENGTH    (8u * 1024 * 1024)
#define USE_MAX           4096
#define SVG_SCAN_MAX      (256u * 1024 * 1024)

typedef struct {
    uint8_t *data;
    size_t length, capacity;
    bool failed;
} bytes_t;

typedef struct {
    bytes_t *out;
    uint8_t block[255];
    int block_length;
    uint32_t bits;
    int bit_count;
} packer_t;

static const uint8_t bayer[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

static void put_bytes(bytes_t *bytes, const void *data, size_t length) {
    if (bytes->failed) return;
    if (bytes->length + length > bytes->capacity) {
        size_t capacity = bytes->capacity ? bytes->capacity : 4096;
        while (capacity < bytes->length + length) capacity *= 2;
        uint8_t *grown = realloc(bytes->data, capacity);
        if (!grown) {
            bytes->failed = true;
            return;
        }
        bytes->data = grown;
        bytes->capacity = capacity;
    }
    memcpy(bytes->data + bytes->length, data, length);
    bytes->length += length;
}

static void put_byte(bytes_t *bytes, uint8_t value) {
    put_bytes(bytes, &value, 1);
}

static void put_u16(bytes_t *bytes, int value) {
    put_byte(bytes, (uint8_t)value);
    put_byte(bytes, (uint8_t)(value >> 8));
}

static void flush_block(packer_t *packer) {
    if (!packer->block_length) return;
    put_byte(packer->out, (uint8_t)packer->block_length);
    put_bytes(packer->out, packer->block, (size_t)packer->block_length);
    packer->block_length = 0;
}

static void pack(packer_t *packer, int code, int size) {
    packer->bits |= (uint32_t)code << packer->bit_count;
    packer->bit_count += size;
    while (packer->bit_count >= 8) {
        packer->block[packer->block_length++] = (uint8_t)packer->bits;
        packer->bits >>= 8;
        packer->bit_count -= 8;
        if (packer->block_length == 255) flush_block(packer);
    }
}

static void finish(packer_t *packer) {
    if (packer->bit_count) packer->block[packer->block_length++] = (uint8_t)packer->bits;
    packer->bits = 0;
    packer->bit_count = 0;
    flush_block(packer);
    put_byte(packer->out, 0);
}

static bool compress(bytes_t *out, const uint8_t *pixels, size_t count) {
    uint16_t (*children)[GREY_LEVELS] = calloc(MAX_CODE + 1, sizeof *children);
    if (!children) return false;
    packer_t packer = { .out = out };
    int code_size = MIN_CODE_SIZE + 1, max_code = END_CODE, current = pixels[0];
    bool first_after_clear = true;
    put_byte(out, MIN_CODE_SIZE);
    pack(&packer, CLEAR_CODE, code_size);
    for (size_t i = 1; i < count; i++) {
        int next = pixels[i];
        if (children[current][next]) {
            current = children[current][next];
            continue;
        }
        pack(&packer, current, code_size);
        first_after_clear = false;
        children[current][next] = (uint16_t)++ max_code;
        if (max_code >= (1 << code_size)) code_size++;
        if (max_code == MAX_CODE) {
            pack(&packer, CLEAR_CODE, code_size);
            memset(children, 0, (MAX_CODE + 1) * sizeof *children);
            code_size = MIN_CODE_SIZE + 1;
            max_code = END_CODE;
            first_after_clear = true;
        }
        current = next;
    }
    pack(&packer, current, code_size);
    if (!first_after_clear && max_code + 1 >= (1 << code_size) && code_size < 12) code_size++;
    pack(&packer, END_CODE, code_size);
    finish(&packer);
    free(children);
    return true;
}

static bool encode_gif(const uint8_t *pixels, int width, int height, uint8_t **gif, size_t *gif_length) {
    bytes_t out = { 0 };
    put_bytes(&out, "GIF89a", 6);
    put_u16(&out, width);
    put_u16(&out, height);
    put_byte(&out, 0x80 | 0x10 | 1);
    put_byte(&out, 0);
    put_byte(&out, 0);
    for (int level = 0; level < GREY_LEVELS; level++) {
        uint8_t grey = (uint8_t)(level * 255 / (GREY_LEVELS - 1));
        uint8_t rgb[3] = { grey, grey, grey };
        put_bytes(&out, rgb, 3);
    }
    put_byte(&out, ',');
    put_u16(&out, 0);
    put_u16(&out, 0);
    put_u16(&out, width);
    put_u16(&out, height);
    put_byte(&out, 0);
    bool compressed = compress(&out, pixels, (size_t)width * (size_t)height);
    put_byte(&out, ';');
    if (!compressed || out.failed) {
        free(out.data);
        return false;
    }
    *gif = out.data;
    *gif_length = out.length;
    return true;
}

static void target_size(float width, float height, int hint_width, int hint_height, int *target_width, int *target_height) {
    if (hint_width > 0 && hint_height > 0) {
        width = (float)hint_width;
        height = (float)hint_height;
    } else if (hint_width > 0) {
        height = height * (float)hint_width / width;
        width = (float)hint_width;
    } else if (hint_height > 0) {
        width = width * (float)hint_height / height;
        height = (float)hint_height;
    }
    float scale = 1.0f;
    if (width * scale > WEB_IMAGE_MAX_WIDTH) scale = WEB_IMAGE_MAX_WIDTH / width;
    if (height * scale > WEB_IMAGE_MAX_HEIGHT) scale = WEB_IMAGE_MAX_HEIGHT / height;
    *target_width = (int)lroundf(width * scale);
    *target_height = (int)lroundf(height * scale);
    if (*target_width < 1) *target_width = 1;
    if (*target_height < 1) *target_height = 1;
}

static const char *find_text(const char *start, const char *end, const char *needle) {
    size_t length = strlen(needle);
    for (const char *p = start; p + length <= end; p++) {
        if (!memcmp(p, needle, length)) return p;
    }
    return NULL;
}

static bool svg_attribute(const char *tag, const char *tag_end, const char *name, char *out, size_t size) {
    size_t length = strlen(name);
    for (const char *p = tag + 1; p + length < tag_end; p++) {
        if (!isspace((unsigned char)p[-1]) || strncmp(p, name, length) || p[length] != '=') continue;
        char quote = p[length + 1];
        if (quote != '"' && quote != '\'') continue;
        const char *value = p + length + 2;
        const char *close = memchr(value, quote, (size_t)(tag_end - value));
        if (!close) return false;
        snprintf(out, size, "%.*s", (int)(close - value), value);
        return true;
    }
    return false;
}

static bool element_name_at(const char *p, const char *end, const char *name) {
    size_t length = strlen(name);
    if (p + length >= end || strncmp(p, name, length)) return false;
    char next = p[length];
    return next == '>' || next == '/' || isspace((unsigned char)next);
}

static bool find_element(const char *text, const char *end, const char *id, const char **start, const char **finish, char *name, size_t name_size, size_t *scanned) {
    char needle[256];
    for (int quote = 0; quote < 2; quote++) {
        snprintf(needle, sizeof needle, quote ? " id='%s'" : " id=\"%s\"", id);
        const char *found = find_text(text, end, needle);
        *scanned += (size_t)((found ? found : end) - text);
        if (!found) continue;
        const char *open = found;
        while (open > text && *open != '<') open--;
        if (*open != '<') return false;
        size_t length = strcspn(open + 1, " \t\r\n/>");
        if (length >= name_size) return false;
        snprintf(name, name_size, "%.*s", (int)length, open + 1);
        const char *tag_end = memchr(open, '>', (size_t)(end - open));
        if (!tag_end) return false;
        *start = open;
        if (tag_end[-1] == '/') {
            *finish = tag_end + 1;
            return true;
        }
        int depth = 1;
        for (const char *p = tag_end + 1; p < end; p++) {
            if (*p != '<') continue;
            if (p[1] == '/' && element_name_at(p + 2, end, name)) {
                if (--depth == 0) {
                    *scanned += (size_t)(p - tag_end);
                    const char *close = memchr(p, '>', (size_t)(end - p));
                    if (!close) return false;
                    *finish = close + 1;
                    return true;
                }
            } else if (element_name_at(p + 1, end, name)) {
                const char *inner_end = memchr(p, '>', (size_t)(end - p));
                if (inner_end && inner_end[-1] != '/') depth++;
            }
        }
        *scanned += (size_t)(end - tag_end);
        return false;
    }
    return false;
}

static void append(bytes_t *out, const char *text, size_t length) {
    put_bytes(out, text, length);
}

static void append_element(bytes_t *out, const char *start, const char *finish, const char *name) {
    if (strcmp(name, "symbol")) {
        append(out, start, (size_t)(finish - start));
        return;
    }
    const char *body = start + 1 + strlen(name);
    const char *close = finish;
    while (close > body && *close != '<') close--;
    append(out, "<g", 2);
    if (close > body && close[1] == '/') {
        append(out, body, (size_t)(close - body));
        append(out, "</g>", 4);
    } else {
        append(out, body, (size_t)(finish - body));
    }
}

static char *expand_uses(const char *text, size_t length) {
    char *current = malloc(length + 1);
    if (!current) return NULL;
    memcpy(current, text, length);
    current[length] = 0;
    size_t scanned = 0;
    int expansions = 0;
    for (int pass = 0; pass < USE_PASSES; pass++) {
        const char *end = current + strlen(current);
        const char *use = current;
        while ((use = find_text(use, end, "<use")) && !element_name_at(use + 1, end, "use")) use += 4;
        if (!use) break;
        bytes_t out = { 0 };
        const char *copied = current;
        char cached_reference[256] = "", cached_name[64] = "";
        const char *cached_start = NULL, *cached_finish = NULL;
        bool cached_found = false;
        for (const char *p = current; !out.failed && (p = find_text(p, end, "<use")); ) {
            if (!element_name_at(p + 1, end, "use")) {
                p += 4;
                continue;
            }
            const char *tag_end = memchr(p, '>', (size_t)(end - p));
            if (!tag_end) break;
            const char *after = tag_end + 1;
            if (tag_end[-1] != '/') {
                const char *close = find_text(after, end, "</use>");
                if (close) after = close + 6;
            }
            append(&out, copied, (size_t)(p - copied));
            char reference[256] = "", transform[512] = "", x[64] = "0", y[64] = "0";
            if (!svg_attribute(p, tag_end, "href", reference, sizeof reference)) svg_attribute(p, tag_end, "xlink:href", reference, sizeof reference);
            svg_attribute(p, tag_end, "transform", transform, sizeof transform);
            svg_attribute(p, tag_end, "x", x, sizeof x);
            svg_attribute(p, tag_end, "y", y, sizeof y);
            if (++expansions > USE_MAX) {
                out.failed = true;
                break;
            }
            if (reference[0] == '#' && strcmp(reference, cached_reference)) {
                snprintf(cached_reference, sizeof cached_reference, "%s", reference);
                cached_found = find_element(current, end, reference + 1, &cached_start, &cached_finish, cached_name, sizeof cached_name, &scanned);
                if (scanned > SVG_SCAN_MAX) {
                    out.failed = true;
                    break;
                }
            }
            if (reference[0] == '#' && cached_found && strcmp(cached_name, "use")) {
                char group[700];
                int group_length = snprintf(group, sizeof group, "<g transform=\"%s translate(%s %s)\">", transform, x, y);
                append(&out, group, (size_t)group_length);
                append_element(&out, cached_start, cached_finish, cached_name);
                append(&out, "</g>", 4);
            }
            if (out.length > SVG_MAX_LENGTH) out.failed = true;
            copied = after;
            p = after;
        }
        append(&out, copied, (size_t)(end - copied));
        append(&out, "", 1);
        free(current);
        if (out.failed || out.length > SVG_MAX_LENGTH) {
            free(out.data);
            return NULL;
        }
        current = (char *)out.data;
    }
    return current;
}

static uint8_t *decode_svg(const uint8_t *data, size_t length, int hint_width, int hint_height, int *width, int *height) {
    char *text = expand_uses((const char *)data, length);
    if (!text) return NULL;
    NSVGimage *image = nsvgParse(text, "px", SVG_DPI);
    free(text);
    if (!image) return NULL;
    uint8_t *rgba = NULL;
    if (image->width > 0 && image->height > 0) {
        target_size(image->width, image->height, hint_width, hint_height, width, height);
        float scale = fminf((float)*width / image->width, (float)*height / image->height);
        NSVGrasterizer *rasterizer = nsvgCreateRasterizer();
        rgba = rasterizer ? calloc((size_t)*width * (size_t)*height, 4) : NULL;
        if (rgba) nsvgRasterize(rasterizer, image, 0, 0, scale, rgba, *width, *height, *width * 4);
        nsvgDeleteRasterizer(rasterizer);
    }
    nsvgDelete(image);
    return rgba;
}

static float *flatten(const uint8_t *rgba, int width, int height) {
    float *grey = malloc((size_t)width * (size_t)height * sizeof *grey);
    if (!grey) return NULL;
    for (size_t i = 0; i < (size_t)width * (size_t)height; i++) {
        const uint8_t *p = rgba + i * 4;
        float luminance = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
        float alpha = p[3] / 255.0f;
        grey[i] = luminance * alpha + 255.0f * (1.0f - alpha);
    }
    return grey;
}

static float *resample(const float *grey, int width, int height, int target_width, int target_height) {
    float *out = malloc((size_t)target_width * (size_t)target_height * sizeof *out);
    if (!out) return NULL;
    for (int ty = 0; ty < target_height; ty++) {
        int y0 = (int)((int64_t)ty * height / target_height);
        int y1 = (int)((int64_t)(ty + 1) * height / target_height);
        if (y1 <= y0) y1 = y0 + 1;
        for (int tx = 0; tx < target_width; tx++) {
            int x0 = (int)((int64_t)tx * width / target_width);
            int x1 = (int)((int64_t)(tx + 1) * width / target_width);
            if (x1 <= x0) x1 = x0 + 1;
            float sum = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) sum += grey[(size_t)y * (size_t)width + (size_t)x];
            }
            out[(size_t)ty * (size_t)target_width + (size_t)tx] = sum / (float)((y1 - y0) * (x1 - x0));
        }
    }
    return out;
}

static uint8_t *dither(const float *grey, int width, int height) {
    uint8_t *levels = malloc((size_t)width * (size_t)height);
    if (!levels) return NULL;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float value = grey[(size_t)y * (size_t)width + (size_t)x] / 255.0f * (GREY_LEVELS - 1);
            int level = (int)value;
            float threshold = (bayer[y & 7][x & 7] + 0.5f) / 64.0f;
            if (value - (float)level > threshold) level++;
            if (level < 0) level = 0;
            if (level > GREY_LEVELS - 1) level = GREY_LEVELS - 1;
            levels[(size_t)y * (size_t)width + (size_t)x] = (uint8_t)level;
        }
    }
    return levels;
}

bool web_image_convert(const uint8_t *data, size_t length, bool svg, int hint_width, int hint_height, uint8_t **gif, size_t *gif_length) {
    int width = 0, height = 0, channels;
    uint8_t *rgba;
    if (svg) rgba = decode_svg(data, length, hint_width, hint_height, &width, &height);
    else if (length > (size_t)INT32_MAX || !stbi_info_from_memory(data, (int)length, &width, &height, &channels) ||
             (uint64_t)width * (uint64_t)height > MAX_SOURCE_PIXELS) rgba = NULL;
    else rgba = stbi_load_from_memory(data, (int)length, &width, &height, &channels, 4);
    if (!rgba || width < 1 || height < 1) {
        free(rgba);
        return false;
    }
    float *grey = flatten(rgba, width, height);
    free(rgba);
    if (!grey) return false;
    int target_width, target_height;
    target_size((float)width, (float)height, hint_width, hint_height, &target_width, &target_height);
    if (target_width != width || target_height != height) {
        float *resampled = resample(grey, width, height, target_width, target_height);
        free(grey);
        if (!resampled) return false;
        grey = resampled;
    }
    uint8_t *levels = dither(grey, target_width, target_height);
    free(grey);
    if (!levels) return false;
    bool encoded = encode_gif(levels, target_width, target_height, gif, gif_length);
    free(levels);
    return encoded;
}

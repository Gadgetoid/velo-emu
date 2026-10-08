#include "util/png.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

typedef struct {
    uint8_t *data;
    size_t length, capacity;
    bool failed;
} buffer_t;

static void append(buffer_t *buffer, const void *bytes, size_t length) {
    if (buffer->failed || !length) return;
    if (buffer->length + length > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity * 2 : 65536;
        while (capacity < buffer->length + length) capacity *= 2;
        uint8_t *grown = realloc(buffer->data, capacity);
        if (!grown) { buffer->failed = true; return; }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, bytes, length);
    buffer->length += length;
}

static void append_u32(buffer_t *buffer, uint32_t value) {
    uint8_t bytes[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };
    append(buffer, bytes, 4);
}

static void append_chunk(buffer_t *buffer, const char *type, const uint8_t *data, size_t length) {
    append_u32(buffer, (uint32_t)length);
    append(buffer, type, 4);
    append(buffer, data, length);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (length) crc = crc32(crc, data, (uInt)length);
    append_u32(buffer, (uint32_t)crc);
}

static bool encode(const uint32_t *pixels, int width, int height, bool alpha, uint8_t **png, size_t *png_length) {
    size_t channels = alpha ? 4 : 3;
    size_t row = (size_t)width * channels + 1;
    size_t raw_length = row * (size_t)height;
    uint8_t *raw = malloc(raw_length);
    if (!raw) return false;
    for (int y = 0; y < height; y++) {
        uint8_t *out = raw + (size_t)y * row;
        *out++ = 0;
        for (int x = 0; x < width; x++) {
            uint32_t pixel = pixels[(size_t)y * width + x];
            *out++ = (uint8_t)pixel;
            *out++ = (uint8_t)(pixel >> 8);
            *out++ = (uint8_t)(pixel >> 16);
            if (alpha) *out++ = (uint8_t)(pixel >> 24);
        }
    }
    uLongf packed_length = compressBound((uLong)raw_length);
    uint8_t *packed = malloc(packed_length);
    bool ok = packed && compress2(packed, &packed_length, raw, (uLong)raw_length, 9) == Z_OK;
    free(raw);
    if (!ok) { free(packed); return false; }

    buffer_t buffer = { 0 };
    static const uint8_t signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    append(&buffer, signature, sizeof signature);
    uint8_t header[13] = {
        (uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
        (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
        8, alpha ? 6 : 2, 0, 0, 0,
    };
    append_chunk(&buffer, "IHDR", header, sizeof header);
    append_chunk(&buffer, "IDAT", packed, packed_length);
    append_chunk(&buffer, "IEND", NULL, 0);
    free(packed);
    if (buffer.failed) { free(buffer.data); return false; }
    *png = buffer.data;
    *png_length = buffer.length;
    return true;
}

bool png_encode(const uint32_t *pixels, int width, int height, uint8_t **png, size_t *png_length) {
    return encode(pixels, width, height, false, png, png_length);
}

bool png_encode_rgba(const uint32_t *pixels, int width, int height, uint8_t **png, size_t *png_length) {
    return encode(pixels, width, height, true, png, png_length);
}

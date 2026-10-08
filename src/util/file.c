#include "util/file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

uint8_t *file_read(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    long length = -1;
    if (fseek(file, 0, SEEK_END) == 0) length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return NULL; }
    uint8_t *data = malloc(length ? (size_t)length : 1);
    if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) { free(data); fclose(file); return NULL; }
    fclose(file);
    *size = (size_t)length;
    return data;
}

const char *file_leaf_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

bool file_has_extension(const char *path, const char *extension) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, extension);
}

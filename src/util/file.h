#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint8_t    *file_read(const char *path, size_t *size);
const char *file_leaf_name(const char *path);
bool        file_has_extension(const char *path, const char *extension);

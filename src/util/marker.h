#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool marker_patch_sets(const uint8_t *image, size_t size, char *sets, size_t sets_size);
bool marker_has_set(const char *sets, const char *name);

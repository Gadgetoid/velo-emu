#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool marker_value(const uint8_t *image, size_t size, const char *key, char *value, size_t value_size);
bool marker_has_set(const char *sets, const char *name);

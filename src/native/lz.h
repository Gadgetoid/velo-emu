#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

uint32_t lz_size(const uint8_t *in, size_t in_size);
#define LZ_WINDOW_BLOCKS 16

long     lz_decode(const uint8_t *in, size_t in_size, size_t skip, uint8_t *out, size_t count);
long     lz_decode_window(const uint8_t *data, uint32_t data_start, size_t data_length, const uint32_t *starts, uint32_t first_block,
                          uint32_t block_count, size_t skip, uint8_t *out, size_t count);
size_t   lz_encode(const uint8_t *in, size_t length, uint8_t *out, size_t out_size, bool *all_zero);

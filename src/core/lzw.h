#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t lzw_decode(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size);
size_t lzw_decode_runs(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size, uint16_t *runs, size_t *run_count, size_t max_runs);
size_t lzw_encode(const uint8_t *in, size_t in_size, size_t stride, uint8_t *out, size_t out_size, bool end_code);

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define NATIVE_ARGUMENTS 6

typedef struct {
    void *context;
    uint8_t *(*map)(void *context, uint32_t va, bool write);
} native_memory_t;

typedef struct {
    uint32_t value;
    bool call_next;
} native_result_t;

typedef bool (*native_fn)(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);

bool native_ce1_decode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_ce1_encode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_ce2_decode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_ce2_encode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_fill32(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_strcmp(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_wcslen(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_widen(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_range_lookup(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_export_lookup(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_strcmp_signed(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_zero(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_memmove(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_range_lookup16(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_return_zero(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result);
bool native_read(const native_memory_t *memory, uint32_t va, uint8_t *data, uint32_t length);

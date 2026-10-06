#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/mips.h"

typedef struct {
    void    *context;
    uint8_t *(*map)(void *context, uint32_t va, bool write);
} accel_memory_t;

bool accel_ce1_find(const uint8_t *rom, uint32_t rom_pa, uint32_t rom_size, uint32_t *decode_va, uint32_t *encode_va);
bool accel_ce1_decode(mips_cpu_t *cpu, const accel_memory_t *memory);
bool accel_ce1_encode(mips_cpu_t *cpu, const accel_memory_t *memory);

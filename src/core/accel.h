#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/mips.h"

typedef struct {
    void    *context;
    uint8_t *(*map)(void *context, uint32_t va, bool write);
} accel_memory_t;

typedef const uint8_t *(*accel_rom_fn)(void *context, uint32_t pa, uint32_t length);

typedef struct {
    int      system;
    uint32_t decode_va, encode_va;
} accel_hooks_t;

bool accel_find(accel_rom_fn rom, void *context, accel_hooks_t *hooks);
bool accel_ce1_decode(mips_cpu_t *cpu, const accel_memory_t *memory);
bool accel_ce1_encode(mips_cpu_t *cpu, const accel_memory_t *memory);
bool accel_ce2_decode(mips_cpu_t *cpu, const accel_memory_t *memory);
bool accel_ce2_encode(mips_cpu_t *cpu, const accel_memory_t *memory);

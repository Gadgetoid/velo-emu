#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/mips.h"
#include "native/native.h"

#define OPTIMISER_HOOKS_MAX 16
#define OPTIMISER_SHADOW_PAGES 256

typedef const uint8_t *(*optimiser_rom_fn)(void *context, uint32_t pa, uint32_t length);

typedef enum { OPTIMISER_UNCHECKED, OPTIMISER_MATCHED, OPTIMISER_MISMATCHED } optimiser_state_t;

typedef struct {
    uint32_t va;
    const uint32_t *code;
    uint32_t words;
    native_fn run;
    uint32_t next;
    bool no_result;
    optimiser_state_t state;
} optimiser_hook_t;

typedef void (*optimiser_log_fn)(void *context, const char *message);

typedef struct {
    bool pending;
    uint32_t hook_va, return_pc, stack, value;
    bool check_value;
    uint32_t arguments[NATIVE_ARGUMENTS];
    native_shadow_t shadow;
    native_shadow_page_t pages[OPTIMISER_SHADOW_PAGES];
    uint32_t checked, differed;
    optimiser_log_fn log;
    void *log_context;
} optimiser_verify_t;

typedef struct {
    native_memory_t memory;
    const char *profile;
    int hook_count;
    optimiser_hook_t hooks[OPTIMISER_HOOKS_MAX];
    optimiser_verify_t *verify;
} optimiser_t;

void optimiser_init(optimiser_t *optimiser, optimiser_rom_fn rom, void *rom_context, native_memory_t memory);
bool optimiser_hooked(const optimiser_t *optimiser, uint32_t pc);
bool optimiser_call(optimiser_t *optimiser, mips_cpu_t *cpu, uint32_t pc);
bool optimiser_set_verify(optimiser_t *optimiser, bool verify, optimiser_log_fn log, void *log_context);
uint32_t optimiser_return_watch(const optimiser_t *optimiser);

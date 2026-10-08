#include "core/optimiser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AREA_MASK       0x1FFFFFFFu
#define STACK_ARGUMENTS 16u
#define CODE_WORDS_MAX  24

static const uint32_t CE1_DECODE_CODE[] = {
    0x27BDFF98u, 0xAFBF0034u, 0xAFB70030u, 0xAFB6002Cu, 0xAFB50028u, 0xAFB40024u,
    0xAFB30020u, 0xAFB2001Cu, 0xAFB10018u, 0xAFB00014u, 0xAFA70074u, 0x8FAE0074u,
    0x3C018001u, 0x8DD20000u, 0xAC247D64u, 0x3C018002u, 0xAC25A594u, 0x3C148002u,
    0x3C138001u, 0x3408FFFFu, 0x00C08025u, 0x26736D60u, 0x2694A5A0u, 0x8FA50054u,
};

static const uint32_t CE1_ENCODE_CODE[] = {
    0x27BDFF90u, 0xAFBF003Cu, 0xAFBE0038u, 0xAFB70034u, 0xAFB60030u, 0xAFB5002Cu,
    0xAFB40028u, 0xAFB30024u, 0xAFB20020u, 0xAFB1001Cu, 0xAFB00018u, 0xAFA7007Cu,
    0x30A5FFFFu, 0x8FAF007Cu, 0x240E0001u, 0x3C018001u, 0xA3AE006Fu, 0xAC267D64u,
    0x95F80000u, 0x3C018002u, 0x3C148002u, 0x3C138001u, 0x3C128001u, 0x30BEFFFFu,
};

static const uint32_t CE2_DECODE_CODE[] = {
    0x27BDFF90u, 0xAFB60038u, 0xAFB40030u, 0xAFB00020u, 0x00A08025u, 0x00E0A025u,
    0x00C0B025u, 0xAFBF0044u, 0xAFBE0040u, 0xAFB7003Cu, 0xAFB50034u, 0xAFB3002Cu,
    0xAFB20028u, 0xAFB10024u, 0xAFA40070u, 0x97A20084u, 0x24010001u, 0x10410003u,
    0x24010002u, 0x5441004Au, 0x2402FFFFu, 0x8FB10080u, 0x2E010003u, 0x322E03FFu,
};

static const uint32_t CE2_ENCODE_CODE[] = {
    0x27BDFFB0u, 0xAFB7003Cu, 0xAFB60038u, 0xAFB50034u, 0xAFB40030u, 0x00A0A025u,
    0x00E0A825u, 0x00C0B025u, 0x0080B825u, 0xAFBF0044u, 0xAFBE0040u, 0xAFB3002Cu,
    0xAFB20028u, 0xAFB10024u, 0xAFB00020u, 0x240E0001u, 0x3C018002u, 0xAC2EEBE0u,
    0x3C010100u, 0x0281082Bu, 0x50200064u, 0x2402FFFFu, 0x12C00011u, 0x2EA10003u,
};

static const uint32_t EXPORT_CE2_CODE[] = {
    0x27BDFFC0u, 0xAFB40028u, 0xAFB30024u, 0x00A09825u, 0x0080A025u, 0xAFBF002Cu,
    0xAFB20020u, 0xAFB1001Cu, 0xAFB00018u, 0x8E86007Cu, 0x50C0002Eu, 0x00001025u,
    0x8E830050u, 0x00008825u, 0x00669021u, 0x8E4F001Cu,
};

static const uint32_t ZERO_CE2_CODE[] = {
    0xAC800000u, 0xAC800004u, 0xAC800008u, 0xAC80000Cu, 0xAC800010u, 0xAC800014u,
    0xAC800018u, 0xAC80001Cu, 0x20A5FFE0u, 0x1405FFF6u, 0x20840020u, 0x03E00008u,
    0x00000000u,
};

static const uint32_t WIDEN_CE2_CODE[] = {
    0x28C10002u, 0x1420000Du, 0x24020001u, 0x90A30000u, 0x1060000Au, 0x00000000u,
    0x24420001u, 0x0046082Au, 0xA4830000u, 0x24840002u, 0x10200004u, 0x24A50001u,
    0x90A30000u, 0x5460FFF9u, 0x24420001u, 0x03E00008u, 0xA4800000u,
};

static const uint32_t MOVE_CE2_CODE[] = {
    0x00801025u, 0x00A4082Bu, 0x10200005u, 0x00000000u, 0x00A64021u, 0x0088082Bu,
    0x142000EBu, 0x00000000u, 0x2CC80004u, 0x1408008Cu, 0x00000000u, 0x00854026u,
    0x31080003u, 0x14080092u, 0x00000000u,
};

static const uint32_t RANGE_CE2_CODE[] = {
    0x27BDFFF8u, 0xAFB00004u, 0x00808025u, 0x30C6FFFFu, 0x10A0001Bu, 0x24A3FFFFu,
    0x04600019u, 0x00001025u, 0x00C03825u, 0x24090003u, 0x00432821u, 0x00052843u,
};

static const uint32_t GPE_BLT_CODE[] = {
    0x27BDFD70u, 0xAFB20030u, 0x00A09025u, 0xAFBF004Cu, 0xAFBE0048u, 0xAFB70044u,
    0xAFB60040u, 0xAFB5003Cu, 0xAFB40038u, 0xAFB30034u, 0xAFB1002Cu, 0xAFB00028u,
};

typedef struct {
    uint32_t va;
    const uint32_t *code;
    uint32_t words;
    native_fn run;
    bool in_rom;
    uint32_t next;
    bool no_result;
} hook_spec_t;

typedef struct {
    const char *name;
    const hook_spec_t *hooks;
    int hook_count;
} profile_t;

#define CODE(code) (code), (uint32_t)(sizeof(code) / sizeof((code)[0]))
#define COUNT(table) (int)(sizeof(table) / sizeof((table)[0]))

static const hook_spec_t CE1_HOOKS[] = {
    { 0x9F41F80Cu, CODE(CE1_DECODE_CODE), native_ce1_decode, true, 0, false },
    { 0x9F41FA94u, CODE(CE1_ENCODE_CODE), native_ce1_encode, true, 0, false },
};

static const hook_spec_t CE2_HOOKS[] = {
    { 0x9005B000u, CODE(CE2_DECODE_CODE), native_ce2_decode, true, 0, false },
    { 0x9005ADECu, CODE(CE2_ENCODE_CODE), native_ce2_encode, true, 0, false },
    { 0x900412A0u, CODE(EXPORT_CE2_CODE), native_export_lookup, true, 0x90043D2Cu, false },
    { 0x900526A0u, CODE(ZERO_CE2_CODE), native_zero, true, 0, true },
    { 0x90057298u, CODE(WIDEN_CE2_CODE), native_widen, true, 0, false },
    { 0x9003C360u, CODE(MOVE_CE2_CODE), native_memmove, true, 0, false },
    { 0x01ED4D9Cu, CODE(RANGE_CE2_CODE), native_range_lookup16, false, 0, false },
    { 0x01FD3D7Cu, CODE(GPE_BLT_CODE), native_gpe_blt, false, 0, false },
    { 0x0197607Cu, CODE(GPE_BLT_CODE), native_gpe_blt, false, 0, false },
};

static const profile_t PROFILES[] = {
    { "Velo 1, CE 1.0", CE1_HOOKS, COUNT(CE1_HOOKS) },
    { "Velo 1, CE 2.0", CE2_HOOKS, COUNT(CE2_HOOKS) },
};

static bool in_rom(optimiser_rom_fn rom, void *context, const hook_spec_t *spec) {
    const uint8_t *bytes = rom(context, spec->va & AREA_MASK, spec->words * 4);
    return bytes && memcmp(bytes, spec->code, spec->words * 4) == 0;
}

void optimiser_init(optimiser_t *optimiser, optimiser_rom_fn rom, void *rom_context, native_memory_t memory) {
    *optimiser = (optimiser_t){ .memory = memory };
    for (int p = 0; p < COUNT(PROFILES); p++) {
        const profile_t *profile = &PROFILES[p];
        if (!in_rom(rom, rom_context, &profile->hooks[0])) continue;
        optimiser->profile = profile->name;
        for (int i = 0; i < profile->hook_count && optimiser->hook_count < OPTIMISER_HOOKS_MAX; i++) {
            const hook_spec_t *spec = &profile->hooks[i];
            if (spec->in_rom && !in_rom(rom, rom_context, spec)) continue;
            optimiser->hooks[optimiser->hook_count++] = (optimiser_hook_t){ spec->va, spec->code, spec->words, spec->run, spec->next, spec->no_result, spec->in_rom ? OPTIMISER_MATCHED : OPTIMISER_UNCHECKED };
        }
        return;
    }
}

bool optimiser_hooked(const optimiser_t *optimiser, uint32_t pc) {
    if (optimiser->verify && optimiser->verify->pending && optimiser->verify->return_pc == pc) return true;
    for (int i = 0; i < optimiser->hook_count; i++) {
        if (optimiser->hooks[i].va == pc) return true;
    }
    return false;
}

static bool code_in_memory(const optimiser_t *optimiser, const optimiser_hook_t *hook, bool *readable) {
    uint8_t bytes[CODE_WORDS_MAX * 4];
    *readable = hook->words <= CODE_WORDS_MAX && native_read(&optimiser->memory, hook->va, bytes, hook->words * 4);
    return *readable && memcmp(bytes, hook->code, hook->words * 4) == 0;
}

static bool mips_arguments(const optimiser_t *optimiser, const mips_cpu_t *cpu, uint32_t *arguments) {
    for (int i = 0; i < 4; i++) arguments[i] = cpu->gpr[4 + i];
    uint8_t stack[(NATIVE_ARGUMENTS - 4) * 4];
    if (!native_read(&optimiser->memory, cpu->gpr[29] + STACK_ARGUMENTS, stack, sizeof stack)) return false;
    for (int i = 4; i < NATIVE_ARGUMENTS; i++) {
        const uint8_t *word = stack + (i - 4) * 4;
        arguments[i] = (uint32_t)word[0] | (uint32_t)word[1] << 8 | (uint32_t)word[2] << 16 | (uint32_t)word[3] << 24;
    }
    return true;
}

static uint32_t slot_relative(uint32_t va) {
    return va < MIPS_SLOT_SIZE * 64 ? va & (MIPS_SLOT_SIZE - 1) : va;
}

static void verify_log(optimiser_verify_t *verify, const char *format, ...) {
    char message[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(message, sizeof message, format, arguments);
    va_end(arguments);
    if (verify->log) verify->log(verify->log_context, message);
}

static void verify_start(optimiser_t *optimiser, const optimiser_hook_t *hook, const mips_cpu_t *cpu, const uint32_t *arguments) {
    optimiser_verify_t *verify = optimiser->verify;
    verify->shadow = (native_shadow_t){ optimiser->memory, verify->pages, 0, OPTIMISER_SHADOW_PAGES };
    native_memory_t shadow_memory = { &verify->shadow, native_shadow_map };
    native_result_t result = { 0, false };
    if (!hook->run(&shadow_memory, arguments, &result) || result.call_next) return;
    verify->pending = true;
    verify->hook_va = hook->va;
    verify->return_pc = slot_relative(cpu->gpr[31]);
    verify->stack = cpu->gpr[29];
    verify->value = result.value;
    verify->check_value = !hook->no_result;
    memcpy(verify->arguments, arguments, sizeof verify->arguments);
}

static void verify_finish(optimiser_verify_t *verify, const mips_cpu_t *cpu) {
    uint32_t va;
    verify->pending = false;
    verify->checked++;
    const uint32_t *arguments = verify->arguments;
    if (verify->check_value && cpu->gpr[2] != verify->value) {
        verify->differed++;
        verify_log(verify, "optimiser: %08X(%08X %08X %08X %08X %08X %08X) returned %08X, native %08X\n", verify->hook_va, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], cpu->gpr[2], verify->value);
    } else if (native_shadow_differs(&verify->shadow, &va)) {
        verify->differed++;
        verify_log(verify, "optimiser: %08X(%08X %08X %08X %08X %08X %08X) memory differs at %08X\n", verify->hook_va, arguments[0], arguments[1], arguments[2], arguments[3], arguments[4], arguments[5], va);
    }
}

bool optimiser_call(optimiser_t *optimiser, mips_cpu_t *cpu, uint32_t pc) {
    optimiser_verify_t *verify = optimiser->verify;
    if (verify && verify->pending && verify->return_pc == pc) {
        if (verify->stack == cpu->gpr[29]) verify_finish(verify, cpu);
        if (!optimiser_hooked(optimiser, pc)) return false;
    }
    for (int i = 0; i < optimiser->hook_count; i++) {
        optimiser_hook_t *hook = &optimiser->hooks[i];
        if (hook->va != pc) continue;
        if (hook->state == OPTIMISER_UNCHECKED) {
            bool readable;
            bool matches = code_in_memory(optimiser, hook, &readable);
            if (!readable) return false;
            hook->state = matches ? OPTIMISER_MATCHED : OPTIMISER_MISMATCHED;
        }
        uint32_t arguments[NATIVE_ARGUMENTS];
        native_result_t result = { 0, false };
        if (hook->state != OPTIMISER_MATCHED || !mips_arguments(optimiser, cpu, arguments)) return false;
        if (verify) {
            if (!verify->pending) verify_start(optimiser, hook, cpu, arguments);
            return false;
        }
        if (!hook->run(&optimiser->memory, arguments, &result)) return false;
        if (result.call_next && !hook->next) return false;
        if (result.call_next) {
            cpu->gpr[4] = arguments[0];
            cpu->gpr[5] = result.value;
            mips_jump(cpu, hook->next);
        } else if (hook->no_result) {
            mips_jump(cpu, cpu->gpr[31]);
        } else {
            mips_return(cpu, result.value);
        }
        return true;
    }
    return false;
}

bool optimiser_set_verify(optimiser_t *optimiser, bool verify, optimiser_log_fn log, void *log_context) {
    free(optimiser->verify);
    optimiser->verify = NULL;
    if (!verify) return true;
    optimiser->verify = calloc(1, sizeof *optimiser->verify);
    if (!optimiser->verify) return false;
    optimiser->verify->log = log;
    optimiser->verify->log_context = log_context;
    return true;
}

uint32_t optimiser_return_watch(const optimiser_t *optimiser) {
    return optimiser->verify && optimiser->verify->pending ? optimiser->verify->return_pc : 0;
}

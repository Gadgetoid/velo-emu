#include "core/accel.h"

#include <string.h>

#include "core/lzw.h"

#define CE1_DECODE_VA 0x9F41F80Cu
#define CE1_ENCODE_VA 0x9F41FA94u
#define CODEC_MAX     0x2000u
#define PAGE          0x1000u

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

static bool code_matches(const uint8_t *rom, uint32_t rom_pa, uint32_t rom_size, uint32_t va, const uint32_t *code, size_t words) {
    uint32_t pa = va & 0x1FFFFFFFu;
    if (pa < rom_pa || pa - rom_pa + words * 4 > rom_size) return false;
    return memcmp(rom + (pa - rom_pa), code, words * 4) == 0;
}

bool accel_ce1_find(const uint8_t *rom, uint32_t rom_pa, uint32_t rom_size, uint32_t *decode_va, uint32_t *encode_va) {
    if (!code_matches(rom, rom_pa, rom_size, CE1_DECODE_VA, CE1_DECODE_CODE, sizeof CE1_DECODE_CODE / 4)) return false;
    if (!code_matches(rom, rom_pa, rom_size, CE1_ENCODE_VA, CE1_ENCODE_CODE, sizeof CE1_ENCODE_CODE / 4)) return false;
    *decode_va = CE1_DECODE_VA;
    *encode_va = CE1_ENCODE_VA;
    return true;
}

static bool guest_read(const accel_memory_t *memory, uint32_t va, uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t chunk = PAGE - (va & (PAGE - 1));
        if (chunk > length) chunk = length;
        const uint8_t *host = memory->map(memory->context, va, false);
        if (!host) return false;
        memcpy(data, host, chunk);
        va += chunk;
        data += chunk;
        length -= chunk;
    }
    return true;
}

static bool guest_writable(const accel_memory_t *memory, uint32_t va, uint32_t length) {
    for (uint32_t at = va & ~(PAGE - 1); at < va + length; at += PAGE) {
        if (!memory->map(memory->context, at < va ? va : at, true)) return false;
    }
    return true;
}

static void guest_write(const accel_memory_t *memory, uint32_t va, const uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t chunk = PAGE - (va & (PAGE - 1));
        if (chunk > length) chunk = length;
        memcpy(memory->map(memory->context, va, true), data, chunk);
        va += chunk;
        data += chunk;
        length -= chunk;
    }
}

static bool guest_word(const accel_memory_t *memory, uint32_t va, uint32_t *value) {
    uint8_t bytes[4];
    if (!guest_read(memory, va, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

bool accel_ce1_decode(mips_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    uint32_t source = cpu->gpr[4], length = cpu->gpr[5], destination = cpu->gpr[6], capacity_va = cpu->gpr[7];
    uint32_t skip, stride, capacity;
    if (!guest_word(memory, cpu->gpr[29] + 16, &skip) || !guest_word(memory, cpu->gpr[29] + 20, &stride)) return false;
    if (!guest_word(memory, capacity_va, &capacity)) return false;
    if (skip || stride != 1 || !destination || !length || length > CODEC_MAX || !capacity || capacity > CODEC_MAX) return false;
    if (!guest_read(memory, source, input, length)) return false;
    size_t produced = lzw_decode(input, length, output, sizeof output);
    if (!produced || produced > capacity) return false;
    if (!guest_writable(memory, destination, (uint32_t)produced) || !guest_writable(memory, capacity_va, 4)) return false;
    guest_write(memory, destination, output, (uint32_t)produced);
    uint8_t written[4] = { (uint8_t)produced, (uint8_t)(produced >> 8), (uint8_t)(produced >> 16), (uint8_t)(produced >> 24) };
    guest_write(memory, capacity_va, written, 4);
    mips_return(cpu, 0);
    return true;
}

bool accel_ce1_encode(mips_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    uint32_t source = cpu->gpr[4], length = cpu->gpr[5] & 0xFFFF, destination = cpu->gpr[6], capacity_va = cpu->gpr[7];
    uint32_t stride_word;
    uint8_t capacity_bytes[2];
    if (!guest_word(memory, cpu->gpr[29] + 16, &stride_word) || !guest_read(memory, capacity_va, capacity_bytes, 2)) return false;
    uint32_t stride = stride_word & 0xFFFF, capacity = (uint32_t)capacity_bytes[0] | (uint32_t)capacity_bytes[1] << 8;
    if (!destination || !stride || !length || !capacity) return false;
    uint32_t span = (length - 1) * stride + 1;
    if (span > CODEC_MAX || !guest_read(memory, source, input, span)) return false;
    bool all_zero = true;
    for (uint32_t i = 0; i < length && all_zero; i++) all_zero = input[i * stride] == 0;
    if (all_zero) return false;
    size_t produced = lzw_encode(input, length, stride, output, capacity, true);
    if (!produced || produced >= capacity) return false;
    if (!guest_writable(memory, destination, (uint32_t)produced) || !guest_writable(memory, capacity_va, 2)) return false;
    guest_write(memory, destination, output, (uint32_t)produced);
    uint8_t written[2] = { (uint8_t)produced, (uint8_t)(produced >> 8) };
    guest_write(memory, capacity_va, written, 2);
    mips_return(cpu, 0);
    return true;
}

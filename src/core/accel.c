#include "core/accel.h"

#include <string.h>

#include "core/lz.h"
#include "core/lzw.h"

#define CE1_DECODE_VA 0x9F41F80Cu
#define CE1_ENCODE_VA 0x9F41FA94u
#define CE2_DECODE_VA 0x9005B000u
#define CE2_ENCODE_VA 0x9005ADECu
#define CE2_DATA_MAX  0x4000u
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

static bool code_matches(accel_rom_fn rom, void *context, uint32_t va, const uint32_t *code, size_t words) {
    const uint8_t *bytes = rom(context, va & 0x1FFFFFFFu, (uint32_t)(words * 4));
    return bytes && memcmp(bytes, code, words * 4) == 0;
}

bool accel_find(accel_rom_fn rom, void *context, accel_hooks_t *hooks) {
    if (code_matches(rom, context, CE1_DECODE_VA, CE1_DECODE_CODE, sizeof CE1_DECODE_CODE / 4) &&
        code_matches(rom, context, CE1_ENCODE_VA, CE1_ENCODE_CODE, sizeof CE1_ENCODE_CODE / 4)) {
        *hooks = (accel_hooks_t){ 1, CE1_DECODE_VA, CE1_ENCODE_VA };
        return true;
    }
    if (code_matches(rom, context, CE2_DECODE_VA, CE2_DECODE_CODE, sizeof CE2_DECODE_CODE / 4) &&
        code_matches(rom, context, CE2_ENCODE_VA, CE2_ENCODE_CODE, sizeof CE2_ENCODE_CODE / 4)) {
        *hooks = (accel_hooks_t){ 2, CE2_DECODE_VA, CE2_ENCODE_VA };
        return true;
    }
    *hooks = (accel_hooks_t){ 0, 0, 0 };
    return false;
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

static bool guest_writable_strided(const accel_memory_t *memory, uint32_t va, uint32_t count, uint32_t stride) {
    if (!count) return true;
    uint32_t last = va + (count - 1) * stride;
    if (stride == 1) return guest_writable(memory, va, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = va + i * stride;
        if ((i == 0 || (at & ~(PAGE - 1)) != ((at - stride) & ~(PAGE - 1))) && !memory->map(memory->context, at, true)) return false;
    }
    return last >= va;
}

bool accel_ce1_decode(mips_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    static uint16_t runs[CODEC_MAX];
    uint32_t source = cpu->gpr[4], length = cpu->gpr[5], destination = cpu->gpr[6], capacity_va = cpu->gpr[7];
    uint32_t skip, stride, capacity;
    if (!guest_word(memory, cpu->gpr[29] + 16, &skip) || !guest_word(memory, cpu->gpr[29] + 20, &stride)) return false;
    if (!guest_word(memory, capacity_va, &capacity)) return false;
    if (!destination || !stride || stride > CODEC_MAX || !length || length > CODEC_MAX || !capacity || capacity > CODEC_MAX) return false;
    if (!guest_read(memory, source, input, length)) return false;
    size_t run_count = 0;
    size_t produced = lzw_decode_runs(input, length, output, sizeof output, runs, &run_count, CODEC_MAX);
    if (!produced) return false;
    uint32_t remaining = capacity, skipping = skip, written = 0;
    for (size_t i = 0; i < run_count; i++) {
        if (runs[i] > remaining) return false;
        uint32_t skipped = skipping < runs[i] ? skipping : runs[i];
        skipping -= skipped;
        remaining -= runs[i] - skipped;
        written += runs[i] - skipped;
    }
    if (written > CODEC_MAX || (uint64_t)written * stride > CODEC_MAX * 2u) return false;
    if (!guest_writable_strided(memory, destination, written, stride) || !guest_writable(memory, capacity_va, 4)) return false;
    const uint8_t *from = output + (produced - written);
    if (stride == 1) {
        guest_write(memory, destination, from, written);
    } else {
        for (uint32_t i = 0; i < written; i++) guest_write(memory, destination + i * stride, from + i, 1);
    }
    uint8_t count[4] = { (uint8_t)written, (uint8_t)(written >> 8), (uint8_t)(written >> 16), (uint8_t)(written >> 24) };
    guest_write(memory, capacity_va, count, 4);
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
    if (!stride || !length || !capacity) return false;
    uint32_t span = (length - 1) * stride + 1;
    if (span > CODEC_MAX || !guest_read(memory, source, input, span)) return false;
    bool all_zero = true;
    for (uint32_t i = 0; i < length && all_zero; i++) all_zero = input[i * stride] == 0;
    if (all_zero) return false;
    size_t produced = lzw_encode(input, length, stride, output, capacity, true);
    if (!produced || produced >= capacity) return false;
    if ((destination && !guest_writable(memory, destination, (uint32_t)produced)) || !guest_writable(memory, capacity_va, 2)) return false;
    if (destination) guest_write(memory, destination, output, (uint32_t)produced);
    uint8_t written[2] = { (uint8_t)produced, (uint8_t)(produced >> 8) };
    guest_write(memory, capacity_va, written, 2);
    mips_return(cpu, 0);
    return true;
}

static uint32_t read24(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16;
}

bool accel_ce2_decode(mips_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t data[CE2_DATA_MAX], output[CODEC_MAX], table[3 * (LZ_WINDOW_BLOCKS + 2)];
    uint32_t source = cpu->gpr[4], length = cpu->gpr[5], destination = cpu->gpr[6], capacity = cpu->gpr[7];
    uint32_t skip, stride;
    uint8_t header[3];
    if (!guest_word(memory, cpu->gpr[29] + 16, &skip) || !guest_word(memory, cpu->gpr[29] + 20, &stride)) return false;
    if (stride != 1 || !destination || !capacity || capacity > CODEC_MAX || length < 3) return false;
    if (!guest_read(memory, source, header, 3)) return false;
    uint32_t size = read24(header), blocks = size / 1024 + 1, header_length = 3 * (blocks + 1);
    if (header_length > length || skip >= size || skip % 1024) return false;
    uint32_t count = size - skip < capacity ? size - skip : capacity;
    uint32_t first = skip / 1024, last = (skip + count - 1) / 1024, span = last - first + 1;
    if (span > LZ_WINDOW_BLOCKS) return false;
    if (!guest_read(memory, source + 3 * first, table, 3 * (span + 1))) return false;
    uint32_t starts[LZ_WINDOW_BLOCKS + 1];
    for (uint32_t i = 0; i <= span; i++) starts[i] = first + i ? read24(table + 3 * i) : header_length;
    if (starts[span] < starts[0] || starts[span] > length || starts[span] - starts[0] > sizeof data) return false;
    if (!guest_read(memory, source + starts[0], data, starts[span] - starts[0])) return false;
    if (lz_decode_window(data, starts[0], starts[span] - starts[0], starts, first, span, skip, output, count) != (long)count) return false;
    if (!guest_writable(memory, destination, count)) return false;
    guest_write(memory, destination, output, count);
    mips_return(cpu, count);
    return true;
}

bool accel_ce2_encode(mips_cpu_t *cpu, const accel_memory_t *memory) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX * 2];
    uint32_t source = cpu->gpr[4], length = cpu->gpr[5], destination = cpu->gpr[6], capacity = cpu->gpr[7];
    uint32_t stride;
    if (!guest_word(memory, cpu->gpr[29] + 16, &stride)) return false;
    if (stride != 1 || !destination || !length || length > CODEC_MAX) return false;
    if (!guest_read(memory, source, input, length)) return false;
    bool all_zero;
    size_t produced = lz_encode(input, length, output, sizeof output, &all_zero);
    if (all_zero || !produced || produced > capacity) return false;
    if (!guest_writable(memory, destination, (uint32_t)produced)) return false;
    guest_write(memory, destination, output, (uint32_t)produced);
    mips_return(cpu, (uint32_t)produced);
    return true;
}

#include "native/native.h"

#include <string.h>

#include "native/lz.h"
#include "native/lzw.h"

#define PAGE         0x400u
#define CODEC_MAX    0x2000u
#define CE2_DATA_MAX 0x4000u
#define FILL_MAX     0x4000000u
#define STRING_MAX   0x10000u
#define RANGE_STEPS  32
#define EXPORTS_MAX  0x10000u
#define MODULE_EXPORTS 124u
#define MODULE_BASE  80u
#define EXPORT_FUNCTIONS 20u
#define EXPORT_NAMES 24u
#define EXPORT_FUNCTION_TABLE 28u
#define EXPORT_NAME_TABLE 32u
#define EXPORT_ORDINAL_TABLE 36u

bool native_read(const native_memory_t *memory, uint32_t va, uint8_t *data, uint32_t length) {
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

static bool guest_writable(const native_memory_t *memory, uint32_t va, uint32_t length) {
    for (uint32_t at = va & ~(PAGE - 1); at < va + length; at += PAGE) {
        if (!memory->map(memory->context, at < va ? va : at, true)) return false;
    }
    return true;
}

static void guest_write(const native_memory_t *memory, uint32_t va, const uint8_t *data, uint32_t length) {
    while (length) {
        uint32_t chunk = PAGE - (va & (PAGE - 1));
        if (chunk > length) chunk = length;
        memcpy(memory->map(memory->context, va, true), data, chunk);
        va += chunk;
        data += chunk;
        length -= chunk;
    }
}

static bool guest_word(const native_memory_t *memory, uint32_t va, uint32_t *value) {
    uint8_t bytes[4];
    if (!native_read(memory, va, bytes, 4)) return false;
    *value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
    return true;
}

static bool guest_writable_strided(const native_memory_t *memory, uint32_t va, uint32_t count, uint32_t stride) {
    if (!count) return true;
    uint32_t last = va + (count - 1) * stride;
    if (stride == 1) return guest_writable(memory, va, count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t at = va + i * stride;
        if ((i == 0 || (at & ~(PAGE - 1)) != ((at - stride) & ~(PAGE - 1))) && !memory->map(memory->context, at, true)) return false;
    }
    return last >= va;
}

bool native_ce1_decode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    static uint16_t runs[CODEC_MAX];
    uint32_t source = arguments[0], length = arguments[1], destination = arguments[2], capacity_va = arguments[3];
    uint32_t skip = arguments[4], stride = arguments[5], capacity;
    if (!guest_word(memory, capacity_va, &capacity)) return false;
    if (!destination || !stride || stride > CODEC_MAX || !length || length > CODEC_MAX || !capacity || capacity > CODEC_MAX) return false;
    if (!native_read(memory, source, input, length)) return false;
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
    result->value = 0;
    return true;
}
bool native_ce1_encode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX];
    uint32_t source = arguments[0], length = arguments[1] & 0xFFFF, destination = arguments[2], capacity_va = arguments[3];
    uint8_t capacity_bytes[2];
    if (!native_read(memory, capacity_va, capacity_bytes, 2)) return false;
    uint32_t stride = arguments[4] & 0xFFFF, capacity = (uint32_t)capacity_bytes[0] | (uint32_t)capacity_bytes[1] << 8;
    if (!stride || !length || !capacity) return false;
    uint32_t span = (length - 1) * stride + 1;
    if (span > CODEC_MAX || !native_read(memory, source, input, span)) return false;
    bool all_zero = true;
    for (uint32_t i = 0; i < length && all_zero; i++) all_zero = input[i * stride] == 0;
    if (all_zero) return false;
    size_t produced = lzw_encode(input, length, stride, output, capacity, true);
    if (!produced || produced >= capacity) return false;
    if ((destination && !guest_writable(memory, destination, (uint32_t)produced)) || !guest_writable(memory, capacity_va, 2)) return false;
    if (destination) guest_write(memory, destination, output, (uint32_t)produced);
    uint8_t written[2] = { (uint8_t)produced, (uint8_t)(produced >> 8) };
    guest_write(memory, capacity_va, written, 2);
    result->value = 0;
    return true;
}
static uint32_t read24(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16;
}

bool native_ce2_decode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    static uint8_t data[CE2_DATA_MAX], output[CODEC_MAX], table[3 * (LZ_WINDOW_BLOCKS + 2)];
    uint32_t source = arguments[0], length = arguments[1], destination = arguments[2], capacity = arguments[3];
    uint32_t skip = arguments[4], stride = arguments[5];
    uint8_t header[3];
    if (stride != 1 || !destination || !capacity || capacity > CODEC_MAX || length < 3) return false;
    if (!native_read(memory, source, header, 3)) return false;
    uint32_t size = read24(header), blocks = size / 1024 + 1, header_length = 3 * (blocks + 1);
    if (header_length > length || skip >= size || skip % 1024) return false;
    uint32_t count = size - skip < capacity ? size - skip : capacity;
    uint32_t first = skip / 1024, last = (skip + count - 1) / 1024, span = last - first + 1;
    if (span > LZ_WINDOW_BLOCKS) return false;
    if (!native_read(memory, source + 3 * first, table, 3 * (span + 1))) return false;
    uint32_t starts[LZ_WINDOW_BLOCKS + 1];
    for (uint32_t i = 0; i <= span; i++) starts[i] = first + i ? read24(table + 3 * i) : header_length;
    if (starts[span] < starts[0] || starts[span] > length || starts[span] - starts[0] > sizeof data) return false;
    if (!native_read(memory, source + starts[0], data, starts[span] - starts[0])) return false;
    if (lz_decode_window(data, starts[0], starts[span] - starts[0], starts, first, span, skip, output, count) != (long)count) return false;
    if (!guest_writable(memory, destination, count)) return false;
    guest_write(memory, destination, output, count);
    result->value = count;
    return true;
}
bool native_ce2_encode(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    static uint8_t input[CODEC_MAX], output[CODEC_MAX * 2];
    uint32_t source = arguments[0], length = arguments[1], destination = arguments[2], capacity = arguments[3];
    uint32_t stride = arguments[4];
    if (stride != 1 || !destination || !length || length > CODEC_MAX) return false;
    if (!native_read(memory, source, input, length)) return false;
    bool all_zero;
    size_t produced = lz_encode(input, length, output, sizeof output, &all_zero);
    if (all_zero || !produced || produced > capacity) return false;
    if (!guest_writable(memory, destination, (uint32_t)produced)) return false;
    guest_write(memory, destination, output, (uint32_t)produced);
    result->value = (uint32_t)produced;
    return true;
}
bool native_fill32(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    uint32_t destination = arguments[0], value = arguments[1], length = arguments[2];
    if ((destination & 3) || (length & 3) || length > FILL_MAX) return false;
    if (!guest_writable(memory, destination, length)) return false;
    uint8_t word[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    for (uint32_t at = destination; at < destination + length;) {
        uint32_t chunk = PAGE - (at & (PAGE - 1));
        if (chunk > destination + length - at) chunk = destination + length - at;
        uint8_t *host = memory->map(memory->context, at, true);
        for (uint32_t i = 0; i < chunk; i += 4) memcpy(host + i, word, 4);
        at += chunk;
    }
    result->value = 0;
    return true;
}
static bool guest_byte(const native_memory_t *memory, uint32_t va, uint8_t *value) {
    return native_read(memory, va, value, 1);
}

bool native_strcmp(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    uint32_t left = arguments[0], right = arguments[1];
    uint8_t a = 0, b = 0;
    uint32_t i = 0;
    for (; i < STRING_MAX; i++) {
        if (!guest_byte(memory, left + i, &a) || !guest_byte(memory, right + i, &b)) return false;
        if (!a || a != b) break;
    }
    if (i == STRING_MAX) return false;
    result->value = (uint32_t)a - b;
    return true;
}
bool native_widen(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    static uint8_t text[STRING_MAX];
    uint32_t destination = arguments[0], source = arguments[1];
    int32_t limit = (int32_t)arguments[2];
    uint32_t count = 0;
    while ((int32_t)(count + 1) < limit) {
        if (count == STRING_MAX || !guest_byte(memory, source + count, &text[count])) return false;
        if (!text[count]) break;
        count++;
    }
    if (!guest_writable(memory, destination, (count + 1) * 2)) return false;
    for (uint32_t i = 0; i <= count; i++) {
        uint8_t wide[2] = { i < count ? text[i] : 0, 0 };
        guest_write(memory, destination + i * 2, wide, 2);
    }
    result->value = 0;
    return true;
}
bool native_range_lookup(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    uint32_t table = arguments[0];
    int32_t character = (int32_t)arguments[2], low = 0, high = (int32_t)arguments[1] - 1;
    uint32_t found = arguments[2];
    if (!arguments[1]) high = -1;
    for (int steps = 0; low <= high; steps++) {
        if (steps == RANGE_STEPS) return false;
        int32_t middle = (low + high) / 2;
        uint8_t entry[6];
        if (!native_read(memory, table + (uint32_t)middle * 6, entry, sizeof entry)) return false;
        int32_t first = entry[0] | entry[1] << 8, last = entry[2] | entry[3] << 8;
        if (character < first) {
            high = middle - 1;
        } else if (character > last) {
            low = middle + 1;
        } else {
            found = (uint16_t)((entry[4] | entry[5] << 8) + (uint32_t)character);
            break;
        }
    }
    result->value = found;
    return true;
}
bool native_wcslen(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    uint32_t start = arguments[0], count = 0;
    for (uint8_t wide[2];; count++) {
        if (count == STRING_MAX || !native_read(memory, start + count * 2, wide, 2)) return false;
        if (!wide[0] && !wide[1]) break;
    }
    result->value = count;
    return true;
}
bool native_return_zero(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    (void)memory;
    (void)arguments;
    result->value = 0;
    return true;
}

static bool names_equal(const native_memory_t *memory, uint32_t left, uint32_t right, bool *equal) {
    for (uint32_t i = 0; i < STRING_MAX; i++) {
        uint8_t a, b;
        if (!guest_byte(memory, left + i, &a) || !guest_byte(memory, right + i, &b)) return false;
        if (a != b) {
            *equal = false;
            return true;
        }
        if (!a) {
            *equal = true;
            return true;
        }
    }
    return false;
}

bool native_export_lookup(const native_memory_t *memory, const uint32_t *arguments, native_result_t *result) {
    uint32_t module = arguments[0], name = arguments[1], exports, base;
    if (!guest_word(memory, module + MODULE_EXPORTS, &exports)) return false;
    if (!exports) {
        result->value = 0;
        return true;
    }
    if (!guest_word(memory, module + MODULE_BASE, &base)) return false;
    uint32_t directory = base + exports, functions, count, function_table, name_table, ordinal_table;
    if (!guest_word(memory, directory + EXPORT_FUNCTIONS, &functions) || !guest_word(memory, directory + EXPORT_NAMES, &count) ||
        !guest_word(memory, directory + EXPORT_FUNCTION_TABLE, &function_table) || !guest_word(memory, directory + EXPORT_NAME_TABLE, &name_table) ||
        !guest_word(memory, directory + EXPORT_ORDINAL_TABLE, &ordinal_table)) {
        return false;
    }
    if (count > EXPORTS_MAX) return false;
    uint32_t index = 0;
    for (; index < count; index++) {
        uint32_t entry;
        bool equal;
        if (!guest_word(memory, base + name_table + index * 4, &entry) || !names_equal(memory, name, base + entry, &equal)) return false;
        if (equal) break;
    }
    if (index == count || index >= functions) {
        result->value = 0;
        return true;
    }
    uint8_t ordinal[2];
    uint32_t address;
    if (!native_read(memory, base + ordinal_table + index * 2, ordinal, 2) || !guest_word(memory, base + function_table + (uint32_t)(ordinal[0] | ordinal[1] << 8) * 4, &address)) return false;
    result->value = address;
    result->call_next = true;
    return true;
}

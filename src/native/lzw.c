#include "native/lzw.h"

#include <string.h>

#define LZW_CODES      4096
#define LZW_CLEAR      256
#define LZW_FIRST_CODE 257

size_t lzw_decode_runs(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size, uint16_t *runs, size_t *run_count, size_t max_runs) {
    static uint16_t prefix[LZW_CODES], length[LZW_CODES];
    static uint8_t suffix[LZW_CODES], first[LZW_CODES];
    for (int i = 0; i < 256; i++) {
        prefix[i] = 0xFFFF;
        suffix[i] = first[i] = (uint8_t)i;
        length[i] = 1;
    }
    size_t bit = 0, produced = 0, total_bits = in_size * 8;
    int next = LZW_FIRST_CODE, width = 9, previous = -1;
    while (bit + (size_t)width <= total_bits && produced < out_size) {
        int code = 0;
        for (int k = 0; k < width; k++, bit++) code |= ((in[bit >> 3] >> (bit & 7)) & 1) << k;
        if (code == LZW_CLEAR) {
            next = LZW_FIRST_CODE;
            width = 9;
            previous = -1;
            continue;
        }
        int entry;
        uint8_t entry_first;
        if (code < next) {
            entry = code;
            entry_first = first[code];
        } else if (code == next && previous >= 0) {
            entry = previous;
            entry_first = first[previous];
        } else {
            return 0;
        }
        size_t entry_length = length[entry] + (code == next ? 1u : 0u);
        if (produced + entry_length > out_size) return 0;
        size_t position = produced + length[entry];
        for (int c = entry; c != 0xFFFF; c = prefix[c]) out[--position] = suffix[c];
        if (code == next) out[produced + length[entry]] = entry_first;
        produced += entry_length;
        if (runs) {
            if (*run_count == max_runs) return 0;
            runs[(*run_count)++] = (uint16_t)entry_length;
        }
        if (previous >= 0 && next < LZW_CODES) {
            prefix[next] = (uint16_t)previous;
            suffix[next] = entry_first;
            first[next] = first[previous];
            length[next] = (uint16_t)(length[previous] + 1);
            next++;
        }
        previous = code;
        if (next >= (1 << width) && width < 12) width++;
    }
    return produced;
}

size_t lzw_decode(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size) {
    return lzw_decode_runs(in, in_size, out, out_size, NULL, NULL, 0);
}

typedef struct {
    uint8_t *out;
    size_t size, bit;
    bool overflow;
} bit_writer_t;

static void emit_code(bit_writer_t *writer, int code, int width) {
    for (int k = 0; k < width; k++, writer->bit++) {
        size_t byte = writer->bit >> 3;
        if (byte >= writer->size) {
            writer->overflow = true;
            return;
        }
        if ((code >> k) & 1) writer->out[byte] |= (uint8_t)(1u << (writer->bit & 7));
    }
}

size_t lzw_encode(const uint8_t *in, size_t in_size, size_t stride, uint8_t *out, size_t out_size, bool end_code) {
    enum { SLOTS = 8192 };
    static int32_t keys[SLOTS];
    static uint16_t codes[SLOTS];
    memset(keys, 0xFF, sizeof keys);
    memset(out, 0, out_size);
    bit_writer_t writer = { out, out_size, 0, false };
    int next = LZW_FIRST_CODE, width = 9, current = -1;
    for (size_t i = 0; i < in_size; i++) {
        uint8_t byte = in[i * stride];
        if (current < 0) {
            current = byte;
            continue;
        }
        int32_t key = current << 8 | byte;
        uint32_t slot = ((uint32_t)key * 2654435761u) % SLOTS;
        while (keys[slot] >= 0 && keys[slot] != key) slot = (slot + 1) % SLOTS;
        if (keys[slot] == key) {
            current = codes[slot];
            continue;
        }
        emit_code(&writer, current, width);
        if (next < LZW_CODES) {
            keys[slot] = key;
            codes[slot] = (uint16_t)next++;
        }
        if (next > (1 << width) && width < 12) width++;
        current = byte;
    }
    if (current >= 0) emit_code(&writer, current, width);
    if (end_code) emit_code(&writer, LZW_CLEAR, width);
    return writer.overflow ? 0 : (writer.bit + 7) / 8;
}

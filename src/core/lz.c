#include "core/lz.h"

#include <string.h>

#define BLOCK          1024
#define BLOCK_OUTPUT   4096
#define HASH_SIZE      2048
#define HASH_MASK      0x7FF
#define CHAIN_LIMIT    32
#define LONGEST_MATCH  272
#define SHORT_DISTANCE 17

static uint32_t read24(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16;
}

static long decode_block(const uint8_t *in, size_t in_size, uint8_t *out) {
    size_t position = 0, produced = 0;
    while (position < in_size) {
        uint8_t flags = in[position++];
        for (int bit = 0; bit < 8 && position < in_size; bit++) {
            if (!(flags & (1u << bit))) {
                if (produced == BLOCK_OUTPUT) return -1;
                out[produced++] = in[position++];
                continue;
            }
            uint8_t token = in[position++];
            uint32_t low = token & 15, high = token >> 4, length, source;
            if (low == 1) {
                length = 2;
                if (produced < high + 2) return -1;
                source = (uint32_t)produced - high - 2;
            } else {
                if (position >= in_size) return -1;
                source = high | (uint32_t)in[position++] << 4;
                if (low == 0) {
                    if (position >= in_size) return -1;
                    length = in[position++] + 17u;
                } else {
                    length = low + 1;
                }
            }
            if (produced + length > BLOCK_OUTPUT) return -1;
            for (uint32_t i = 0; i < length; i++) {
                if (source + i >= produced) return -1;
                out[produced] = out[source + i];
                produced++;
            }
        }
    }
    return (long)produced;
}

uint32_t lz_size(const uint8_t *in, size_t in_size) {
    return in_size < 3 ? 0 : read24(in);
}

long lz_decode_window(const uint8_t *data, uint32_t data_start, size_t data_length, const uint32_t *starts, uint32_t first_block,
                      uint32_t block_count, size_t skip, uint8_t *out, size_t count) {
    static uint8_t block[BLOCK_OUTPUT];
    size_t written = 0;
    for (uint32_t i = 0; written < count && i < block_count; i++) {
        uint32_t index = first_block + i, start = starts[i], end = starts[i + 1];
        if (end < start || start < data_start || end - data_start > data_length) return -1;
        long produced = decode_block(data + (start - data_start), end - start, block);
        if (produced < 0) return -1;
        size_t block_start = (size_t)index * BLOCK;
        size_t from = skip + written - block_start;
        if ((size_t)produced <= from) return -1;
        size_t take = (size_t)produced - from;
        if (take > count - written) take = count - written;
        memcpy(out + written, block + from, take);
        written += take;
    }
    return written == count ? (long)count : -1;
}

long lz_decode(const uint8_t *in, size_t in_size, size_t skip, uint8_t *out, size_t count) {
    if (in_size < 3) return -1;
    uint32_t size = read24(in), blocks = size / BLOCK + 1, header = 3 * (blocks + 1);
    if (header > in_size || skip > size) return -1;
    if (count > size - skip) count = size - skip;
    if (!count) return 0;
    uint32_t first = (uint32_t)(skip / BLOCK), last = (uint32_t)((skip + count - 1) / BLOCK);
    if (last >= blocks) return -1;
    uint32_t starts[LZ_WINDOW_BLOCKS + 1];
    long total = 0;
    while (first <= last) {
        uint32_t span = last - first + 1 > LZ_WINDOW_BLOCKS ? LZ_WINDOW_BLOCKS : last - first + 1;
        for (uint32_t i = 0; i <= span; i++) starts[i] = first + i ? read24(in + 3 * (first + i)) : header;
        size_t part = (size_t)(first + span) * BLOCK - skip;
        if (part > count) part = count;
        long got = lz_decode_window(in, 0, in_size, starts, first, span, skip, out, part);
        if (got < 0) return -1;
        total += got;
        out += got;
        skip += (size_t)got;
        count -= (size_t)got;
        first += span;
    }
    return total;
}

typedef struct {
    const uint8_t *block;
    size_t   length, position;
    int16_t  heads[HASH_SIZE];
    uint16_t chain_source[BLOCK];
    int16_t  chain_next[BLOCK];
    int      chain_count;
    uint8_t *out;
    size_t   out_size, produced, flag_offset;
    int      flag_bit;
    bool     all_zero, overflow;
} encoder_t;

static uint32_t hash_pair(uint8_t first, uint8_t second) {
    return ((uint32_t)first ^ second ^ ((uint32_t)first << 5) ^ ((uint32_t)second << 3)) & HASH_MASK;
}

static void insert(encoder_t *e) {
    if (e->position < 2) return;
    size_t start = e->position - 2;
    uint32_t key = hash_pair(e->block[start], e->block[start + 1]);
    e->chain_source[e->chain_count] = (uint16_t)start;
    e->chain_next[e->chain_count] = e->heads[key];
    e->heads[key] = (int16_t)e->chain_count++;
}

static size_t find_match(encoder_t *e, size_t *best_source) {
    const uint8_t *block = e->block;
    size_t position = e->position;
    if (e->length < position + 2) return 0;
    int node = e->heads[hash_pair(block[position], block[position + 1])];
    if (node < 0) return 0;
    size_t end = position + LONGEST_MATCH < e->length ? position + LONGEST_MATCH : e->length;
    size_t best_length = 0, best = 0;
    bool best_all_zero = true;
    for (int step = 0; step < CHAIN_LIMIT; step++) {
        size_t source = e->chain_source[node];
        if (position - source < end - position) end = position + (position - source);
        size_t length = 0;
        bool all_zero = true;
        while (position + length < end && block[position + length] == block[source + length]) {
            if (block[position + length]) all_zero = false;
            length++;
        }
        if (length == 1 || (length == 2 && position - source > SHORT_DISTANCE)) length = 0;
        if (length > best_length) {
            best_length = length;
            best = source;
            best_all_zero = all_zero;
        }
        node = e->chain_next[node];
        if (node < 0) break;
    }
    if (best_length < 2) return 0;
    if (!best_all_zero) e->all_zero = false;
    *best_source = best;
    return best_length;
}

static void put(encoder_t *e, uint8_t byte) {
    if (e->produced == e->out_size) {
        e->overflow = true;
        return;
    }
    e->out[e->produced++] = byte;
}

static void emit(encoder_t *e, bool literal, uint8_t value, size_t length, size_t source) {
    if (e->flag_bit == 0) {
        e->flag_offset = e->produced;
        put(e, 0);
    }
    if (literal) {
        put(e, value);
    } else {
        if (e->flag_offset < e->produced) e->out[e->flag_offset] |= (uint8_t)(1u << e->flag_bit);
        if (length == 2) {
            put(e, (uint8_t)((e->position - source - 2) << 4 | 1));
        } else if (length <= 16) {
            put(e, (uint8_t)((source << 4 | (length - 1)) & 0xFF));
            put(e, (uint8_t)(source >> 4));
        } else {
            put(e, (uint8_t)((source << 4) & 0xFF));
            put(e, (uint8_t)(source >> 4));
            put(e, (uint8_t)(length - 17));
        }
    }
    e->flag_bit = (e->flag_bit + 1) % 8;
}

static bool encode_block(encoder_t *e) {
    while (e->position < e->length && !e->overflow) {
        insert(e);
        size_t source = 0, length = find_match(e, &source);
        if (!length) {
            if (e->block[e->position]) e->all_zero = false;
            emit(e, true, e->block[e->position], 0, 0);
            e->position++;
            continue;
        }
        emit(e, false, 0, length, source);
        e->position++;
        for (size_t i = 1; i < length; i++) {
            insert(e);
            e->position++;
        }
    }
    return !e->overflow;
}

size_t lz_encode(const uint8_t *in, size_t length, uint8_t *out, size_t out_size, bool *all_zero) {
    static encoder_t encoder;
    *all_zero = true;
    if (!length) return 0;
    uint32_t blocks = (uint32_t)(length / BLOCK) + 1, header = 3 * (blocks + 1);
    if (out_size < header || length > 0xFFFFFFu) return 0;
    out[0] = (uint8_t)length;
    out[1] = (uint8_t)(length >> 8);
    out[2] = (uint8_t)(length >> 16);
    size_t body = header;
    for (uint32_t index = 0; index < blocks; index++) {
        encoder_t *e = &encoder;
        size_t start = (size_t)index * BLOCK;
        memset(e->heads, 0xFF, sizeof e->heads);
        e->block = in + start;
        e->length = length - start < BLOCK ? length - start : BLOCK;
        e->position = 0;
        e->chain_count = 0;
        e->out = out + body;
        e->out_size = out_size - body;
        e->produced = 0;
        e->flag_bit = 0;
        e->all_zero = true;
        e->overflow = false;
        if (!encode_block(e)) return 0;
        body += e->produced;
        *all_zero = *all_zero && e->all_zero;
        uint32_t end = (uint32_t)body;
        out[3 * (index + 1)] = (uint8_t)end;
        out[3 * (index + 1) + 1] = (uint8_t)(end >> 8);
        out[3 * (index + 1) + 2] = (uint8_t)(end >> 16);
    }
    return body;
}

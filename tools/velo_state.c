#include "util/options.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define STATE_MAGIC        "VELO1 STATE v2"
#define STATE_HEADER_SIZE  32
#define REMOTE_HOME        "\\My Documents"
#define CARD_DRAM_PA       0x02000000u
#define PAGE_SIZE          0x1000u
#define LOG_MAGIC_OFFSET   4
#define LOG_SECTIONS       0x10
#define LOG_REGISTRY_ROOTS 0x9a
#define HEAP_OFFSET        0x100
#define HANDLES_PER_BLOCK  1024
#define MAX_HANDLE_BLOCKS  64
#define RECORD_MAX         (0xFFFF + 4)
#define NAME_MAX_UTF8      1024
#define CHUNK_SIZE         4096
#define SUBBLOCK_SIZE      1024
#define VALUE_DATA_MAX     4096
#define LIST_MAX           65536
#define DEPTH_MAX          64
#define LZW_CLEAR          0x100
#define LZW_FIRST          0x101
#define LZW_MAX_BITS       12
#define LZW_CODES          (1 << LZW_MAX_BITS)
#define CE1_REGISTRY       "\\Windows\\Pegreg.reg"
#define CE1_BLOCK_SIZE     0x1000
#define CE1_BLOCK_HEADER   0x18
#define CE1_ROOT_MARKER    0x08

enum {
    OBJECT_FILE_INFO = 3,
    OBJECT_FOLDER    = 4,
    OBJECT_FILE      = 5,
    OBJECT_CHUNK     = 6,
    OBJECT_ROOTS     = 0xb,
    OBJECT_KEY       = 0xc,
    OBJECT_VALUE     = 0xd,
};

enum { REG_SZ = 1, REG_DWORD = 4, REG_MULTI_SZ = 7 };

enum { CE1_KEY = 4, CE1_VALUE = 5 };

static const char *usage =
    "usage: velo-state STATE COMMAND [ARGUMENTS]\n"
    "Reads the RAM object store of a saved Velo state (Windows CE 1.0 or 2.0) without running it.\n"
    "\n"
    "  ls [PATH]                list a folder (default \\My Documents)\n"
    "  get PATH [LOCAL]         copy a file out of the state\n"
    "  reg ls|dump KEY          list a registry key, or everything under it\n"
    "  reg get KEY NAME         read a value\n"
    "  diff OTHER               registry values and files that differ in OTHER, a second state\n"
    "\n"
    "Paths and keys are as velo-rapi takes them: relative to \\My Documents unless they start with / or \\, and / and \\ both separate.\n"
    "Only files in RAM are listed, not those in ROM. On CE 1.0 the registry is read from \\Windows\\Pegreg.reg, as filesys last wrote it.\n";

typedef struct {
    uint8_t *dram;
    uint32_t dram_size;
    uint8_t *card;
    uint32_t card_size;
    uint32_t log;
    uint32_t section_va[2];
    uint32_t section_size[2];
    int      section_count;
    uint32_t handle_block[MAX_HANDLE_BLOCKS];
    int      handle_block_count;
    uint16_t registry_roots;
    bool     ce2;
    uint8_t *registry;
    uint32_t registry_size;
} store_t;

typedef struct {
    int      type;
    uint32_t size;
    uint8_t  data[RECORD_MAX];
} record_t;

typedef struct {
    int      type;
    uint16_t id, parent, sibling, child;
    uint16_t attributes;
    char     name[NAME_MAX_UTF8];
} node_t;

typedef struct {
    char   **lines;
    size_t   count, capacity;
} lines_t;

static uint16_t get16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t get24(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t utf16_to_utf8(const uint8_t *text, size_t units, char *out, size_t size) {
    size_t used = 0;
    for (size_t i = 0; i < units && used + 4 < size; i++) {
        uint32_t code = get16(text + 2 * i);
        if (code == 0) break;
        if (code >= 0xD800 && code < 0xDC00 && i + 1 < units) {
            uint32_t low = get16(text + 2 * i + 2);
            if (low >= 0xDC00 && low < 0xE000) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }
        if (code < 0x80) out[used++] = (char)code;
        else if (code < 0x800) {
            out[used++] = (char)(0xC0 | code >> 6);
            out[used++] = (char)(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out[used++] = (char)(0xE0 | code >> 12);
            out[used++] = (char)(0x80 | (code >> 6 & 0x3F));
            out[used++] = (char)(0x80 | (code & 0x3F));
        } else {
            out[used++] = (char)(0xF0 | code >> 18);
            out[used++] = (char)(0x80 | (code >> 12 & 0x3F));
            out[used++] = (char)(0x80 | (code >> 6 & 0x3F));
            out[used++] = (char)(0x80 | (code & 0x3F));
        }
    }
    out[used] = 0;
    return used;
}

static uint8_t *read_gzip(const char *path, size_t *length) {
    gzFile file = gzopen(path, "rb");
    if (!file) return NULL;
    size_t capacity = 1 << 22, used = 0;
    uint8_t *data = malloc(capacity);
    while (data) {
        if (used == capacity) {
            uint8_t *grown = realloc(data, capacity * 2);
            if (!grown) {
                free(data);
                data = NULL;
                break;
            }
            data = grown;
            capacity *= 2;
        }
        int got = gzread(file, data + used, (unsigned)(capacity - used));
        if (got < 0) {
            free(data);
            data = NULL;
            break;
        }
        if (got == 0) break;
        used += (size_t)got;
    }
    gzclose(file);
    *length = used;
    return data;
}

static bool load_memory(const char *path, store_t *store) {
    size_t length;
    uint8_t *contents = read_gzip(path, &length);
    if (!contents) {
        fprintf(stderr, "velo-state: can't read %s\n", path);
        return false;
    }
    if (length < STATE_HEADER_SIZE || memcmp(contents, STATE_MAGIC, sizeof STATE_MAGIC)) {
        fprintf(stderr, "velo-state: %s is not a Velo state\n", path);
        free(contents);
        return false;
    }
    const uint8_t *cursor = contents + STATE_HEADER_SIZE, *end = contents + length;
    while (cursor < end && *cursor) {
        uint8_t name_length = *cursor++;
        if (end - cursor < name_length + 4) break;
        char name[256];
        memcpy(name, cursor, name_length);
        name[name_length] = 0;
        cursor += name_length;
        uint32_t size = get32(cursor);
        cursor += 4;
        if ((uint64_t)(end - cursor) < size) break;
        uint8_t **target = !strcmp(name, "dram") ? &store->dram : !strcmp(name, "dram_card") ? &store->card : NULL;
        if (target && size) {
            *target = malloc(size);
            if (!*target) break;
            memcpy(*target, cursor, size);
            if (target == &store->dram) store->dram_size = size;
            else store->card_size = size;
        }
        cursor += size;
    }
    free(contents);
    if (!store->dram) {
        fprintf(stderr, "velo-state: %s has no memory in it\n", path);
        return false;
    }
    return true;
}

static const uint8_t *physical(const store_t *store, uint32_t va, uint32_t length) {
    uint32_t pa = va & 0x1FFFFFFFu;
    if (pa < CARD_DRAM_PA) return pa + length <= store->dram_size ? store->dram + pa : NULL;
    pa -= CARD_DRAM_PA;
    return pa + length <= store->card_size ? store->card + pa : NULL;
}

static bool store_read(const store_t *store, uint32_t offset, void *out, uint32_t length) {
    uint8_t *to = out;
    for (int i = 0; i < store->section_count && length; i++) {
        if (offset >= store->section_size[i]) {
            offset -= store->section_size[i];
            continue;
        }
        uint32_t take = store->section_size[i] - offset < length ? store->section_size[i] - offset : length;
        const uint8_t *from = physical(store, store->section_va[i] + offset, take);
        if (!from) return false;
        memcpy(to, from, take);
        to += take;
        length -= take;
        offset = 0;
    }
    return length == 0;
}

static bool open_store(const char *path, store_t *store) {
    memset(store, 0, sizeof *store);
    if (!load_memory(path, store)) return false;
    static const uint8_t magic[8] = { 'E', 'K', 'I', 'M', 'E', 'K', 'I', 'M' };
    bool found = false;
    for (uint32_t page = 0; page + PAGE_SIZE <= store->dram_size && !found; page += PAGE_SIZE) {
        if (get32(store->dram + page) == 1 && !memcmp(store->dram + page + LOG_MAGIC_OFFSET, magic, sizeof magic)) {
            store->log = page;
            found = true;
        }
    }
    if (!found) {
        fprintf(stderr, "velo-state: no object store in %s\n", path);
        return false;
    }
    for (int i = 0; i < 2; i++) {
        uint32_t va = get32(store->dram + store->log + LOG_SECTIONS + 16 * i);
        uint32_t size = get32(store->dram + store->log + LOG_SECTIONS + 16 * i + 4);
        if (!size) continue;
        store->section_va[store->section_count] = va;
        store->section_size[store->section_count++] = size;
    }
    uint8_t header[4 * MAX_HANDLE_BLOCKS];
    if (!store->section_count || !store_read(store, 0, header, sizeof header)) {
        fprintf(stderr, "velo-state: the object store in %s is out of range\n", path);
        return false;
    }
    store->handle_block[store->handle_block_count++] = 0;
    for (int i = 1; i < MAX_HANDLE_BLOCKS && get32(header + 4 * i); i++) store->handle_block[store->handle_block_count++] = get32(header + 4 * i);
    store->registry_roots = get16(store->dram + store->log + LOG_REGISTRY_ROOTS);
    return true;
}

static bool read_object(const store_t *store, uint32_t id, record_t *record) {
    uint32_t block = id / HANDLES_PER_BLOCK;
    if (!id && block) return false;
    if (block >= (uint32_t)store->handle_block_count) return false;
    uint8_t entry[4];
    if (!store_read(store, HEAP_OFFSET + store->handle_block[block] + 8 + 4 * (id % HANDLES_PER_BLOCK), entry, 4)) return false;
    uint32_t handle = get32(entry);
    if (!(handle & 1)) return false;
    uint32_t offset = HEAP_OFFSET + (handle & 0xFFFFFCu);
    uint8_t header[4];
    if (!store_read(store, offset, header, 4)) return false;
    record->size = get16(header);
    record->type = header[3] & 0x0F;
    return store_read(store, offset + 4, record->data, record->size);
}

static bool read_node(const store_t *store, uint16_t id, node_t *node) {
    static record_t record;
    if (!read_object(store, id, &record) || (record.type != OBJECT_FOLDER && record.type != OBJECT_FILE) || record.size < 20) return false;
    node->type = record.type;
    node->id = get16(record.data);
    node->parent = get16(record.data + 2);
    node->sibling = get16(record.data + 4);
    node->child = get16(record.data + 6);
    node->attributes = get16(record.data + 16);
    uint32_t units = get16(record.data + 18);
    if (20 + 2 * units > record.size) units = (record.size - 20) / 2;
    utf16_to_utf8(record.data + 20, units, node->name, sizeof node->name);
    return true;
}

static uint32_t file_size(const store_t *store, const node_t *node) {
    static record_t record;
    if (node->type != OBJECT_FILE || !node->child || !read_object(store, node->child, &record) || record.type != OBJECT_FILE_INFO || record.size < 8) return 0;
    return ((uint32_t)get16(record.data + 2) << 12 | get16(record.data + 4)) + 1;
}

static uint32_t expand_subblock(const uint8_t *in, uint32_t in_length, uint8_t *out, uint32_t want) {
    uint32_t at = 0, produced = 0;
    while (at < in_length && produced < want) {
        uint8_t flags = in[at++];
        for (int bit = 0; bit < 8 && at < in_length && produced < want; bit++) {
            if (!(flags >> bit & 1)) {
                out[produced++] = in[at++];
                continue;
            }
            uint8_t token = in[at];
            uint32_t length, source;
            if ((token & 0xF) == 1) {
                length = 2;
                if ((uint32_t)(token >> 4) + 2 > produced) return 0;
                source = produced - ((uint32_t)(token >> 4) + 2);
                at += 1;
            } else {
                if (at + 2 > in_length) return 0;
                source = (uint32_t)get16(in + at) >> 4;
                length = (token & 0xF) + 1u;
                at += 2;
                if ((token & 0xF) == 0) {
                    if (at >= in_length) return 0;
                    length = in[at++] + 17u;
                }
            }
            for (uint32_t i = 0; i < length && produced < want; i++) {
                if (source + i >= produced) return 0;
                out[produced] = out[source + i];
                produced++;
            }
        }
    }
    return produced;
}

static uint8_t lzw_head(const uint16_t *prefix, uint32_t code) {
    while (code >= LZW_FIRST) code = prefix[code];
    return (uint8_t)code;
}

static uint32_t expand_lzw(const uint8_t *in, uint32_t in_length, uint8_t *out, uint32_t want) {
    static uint16_t prefix[LZW_CODES];
    static uint8_t suffix[LZW_CODES];
    uint8_t stack[LZW_CODES + 1];
    uint32_t bit = 0, width = 9, next = LZW_FIRST, produced = 0, previous = 0;
    bool have_previous = false;
    while (bit + width <= in_length * 8 && produced < want) {
        uint32_t code = 0;
        for (uint32_t i = 0; i < width; i++, bit++) code |= (uint32_t)(in[bit >> 3] >> (bit & 7) & 1) << i;
        if (code == LZW_CLEAR) {
            width = 9;
            next = LZW_FIRST;
            have_previous = false;
            continue;
        }
        uint32_t depth = 0, walk = code;
        if (code == next && have_previous) {
            stack[depth++] = lzw_head(prefix, previous);
            walk = previous;
        } else if (code >= next) return 0;
        while (walk >= LZW_FIRST) {
            stack[depth++] = suffix[walk];
            walk = prefix[walk];
        }
        stack[depth++] = (uint8_t)walk;
        uint8_t head = stack[depth - 1];
        while (depth && produced < want) out[produced++] = stack[--depth];
        if (have_previous && next < LZW_CODES) {
            prefix[next] = (uint16_t)previous;
            suffix[next] = head;
            next++;
        }
        if (next >= (1u << width) && width < LZW_MAX_BITS) width++;
        previous = code;
        have_previous = true;
    }
    return produced;
}

static bool expand_chunk(const uint8_t *in, uint32_t in_length, uint8_t *out, uint32_t *out_length) {
    if (in_length < 3) return false;
    uint32_t total = get24(in);
    uint32_t subblocks = total / SUBBLOCK_SIZE + 1;
    if (total > CHUNK_SIZE || 3 + 3 * subblocks > in_length) return false;
    uint32_t start = 3 + 3 * subblocks;
    for (uint32_t i = 0; i < subblocks; i++) {
        uint32_t end = get24(in + 3 + 3 * i);
        uint32_t want = total - i * SUBBLOCK_SIZE < SUBBLOCK_SIZE ? total - i * SUBBLOCK_SIZE : SUBBLOCK_SIZE;
        if (end < start || end > in_length) return false;
        uint8_t *to = out + i * SUBBLOCK_SIZE;
        uint32_t produced = expand_subblock(in + start, end - start, to, want);
        if (end > start && !produced && want) return false;
        memset(to + produced, 0, want - produced);
        start = end;
    }
    *out_length = total;
    return true;
}

static uint8_t *read_file(const store_t *store, const node_t *node, uint32_t *length) {
    static record_t info, chunk;
    *length = file_size(store, node);
    uint8_t *data = malloc(*length ? *length : 1);
    if (!data || !*length) return data;
    if (!read_object(store, node->child, &info)) {
        free(data);
        return NULL;
    }
    uint32_t chunks = get16(info.data + 2) + 1u, at = 0;
    for (uint32_t i = 0; i < chunks && at < *length; i++) {
        if (8 + 2 * i + 2 <= info.size && get16(info.data + 8 + 2 * i) == 0) {
            uint32_t take = *length - at < CHUNK_SIZE ? *length - at : CHUNK_SIZE;
            memset(data + at, 0, take);
            at += take;
            continue;
        }
        if (8 + 2 * i + 2 > info.size || !read_object(store, get16(info.data + 8 + 2 * i), &chunk) || chunk.type != OBJECT_CHUNK || chunk.size < 4) {
            free(data);
            return NULL;
        }
        uint16_t flags = get16(chunk.data + 2);
        uint32_t padding = flags >> 12;
        if (padding + 4 > chunk.size) padding = 0;
        const uint8_t *payload = chunk.data + 4;
        uint32_t payload_length = chunk.size - 4 - padding;
        uint8_t expanded[CHUNK_SIZE];
        uint32_t expanded_length = payload_length;
        if ((flags & 0xFF) == 1 && !store->ce2) {
            expanded_length = expand_lzw(payload, payload_length, expanded, CHUNK_SIZE);
            payload = expanded;
        } else if ((flags & 0xFF) == 1) {
            if (!expand_chunk(payload, payload_length, expanded, &expanded_length)) {
                free(data);
                return NULL;
            }
            payload = expanded;
        }
        uint32_t take = *length - at < expanded_length ? *length - at : expanded_length;
        memcpy(data + at, payload, take);
        at += take;
    }
    return data;
}

static void velo_path(const char *path, char *out, size_t size) {
    if (path[0] == '/' || path[0] == '\\') snprintf(out, size, "%s", path);
    else if (*path) snprintf(out, size, "%s\\%s", REMOTE_HOME, path);
    else snprintf(out, size, "%s", REMOTE_HOME);
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    size_t length = strlen(out);
    while (length > 1 && out[length - 1] == '\\') out[--length] = 0;
}

static bool find_child(const store_t *store, uint16_t folder, const char *name, size_t name_length, node_t *found) {
    node_t parent;
    if (!read_node(store, folder, &parent) || parent.type != OBJECT_FOLDER) return false;
    uint32_t steps = 0;
    for (uint16_t id = parent.child; id && steps++ < LIST_MAX && read_node(store, id, found); id = found->sibling) {
        if (strlen(found->name) == name_length && !strncasecmp(found->name, name, name_length)) return true;
    }
    return false;
}

static bool find_path(const store_t *store, const char *path, node_t *node) {
    if (!read_node(store, 0, node)) return false;
    const char *at = path;
    while (*at) {
        while (*at == '\\') at++;
        if (!*at) break;
        size_t length = strcspn(at, "\\");
        if (!find_child(store, node->id, at, length, node)) return false;
        at += length;
    }
    return true;
}

static bool open_state(const char *path, store_t *store) {
    if (!open_store(path, store)) return false;
    static record_t record;
    store->ce2 = read_object(store, store->registry_roots, &record) && record.type == OBJECT_ROOTS;
    node_t node;
    if (!store->ce2 && find_path(store, CE1_REGISTRY, &node) && node.type == OBJECT_FILE) store->registry = read_file(store, &node, &store->registry_size);
    return true;
}

static bool wildcard(const char *pattern, const char *text) {
    if (!*pattern) return !*text;
    if (*pattern == '*') return wildcard(pattern + 1, text) || (*text && wildcard(pattern, text + 1));
    if (!*text) return false;
    if (*pattern != '?' && tolower((unsigned char)*pattern) != tolower((unsigned char)*text)) return false;
    return wildcard(pattern + 1, text + 1);
}

static void print_node(const store_t *store, const node_t *node) {
    if (node->type == OBJECT_FOLDER) printf("%10s  %s\\\n", "", node->name);
    else printf("%10u  %s\n", file_size(store, node), node->name);
}

static int list(const store_t *store, const char *argument) {
    char path[1100];
    velo_path(argument, path, sizeof path);
    node_t node;
    const char *pattern = NULL;
    if (strpbrk(path, "*?")) {
        char *slash = strrchr(path, '\\');
        *slash = 0;
        pattern = slash + 1;
    }
    if (!find_path(store, path, &node)) {
        fprintf(stderr, "velo-state: no %s\n", path);
        return 1;
    }
    if (node.type == OBJECT_FILE) {
        print_node(store, &node);
        return 0;
    }
    node_t child;
    uint32_t steps = 0;
    for (uint16_t id = node.child; id && steps++ < LIST_MAX && read_node(store, id, &child); id = child.sibling) {
        if (!pattern || wildcard(pattern, child.name)) print_node(store, &child);
    }
    return 0;
}

static int get(const store_t *store, const char *argument, const char *local) {
    char path[1100];
    velo_path(argument, path, sizeof path);
    node_t node;
    if (!find_path(store, path, &node) || node.type != OBJECT_FILE) {
        fprintf(stderr, "velo-state: no file %s\n", path);
        return 1;
    }
    uint32_t length;
    uint8_t *data = read_file(store, &node, &length);
    if (!data) {
        fprintf(stderr, "velo-state: %s is damaged\n", path);
        return 1;
    }
    const char *target = local ? local : node.name;
    FILE *file = fopen(target, "wb");
    bool ok = file && fwrite(data, 1, length, file) == length;
    if (file && fclose(file)) ok = false;
    free(data);
    if (!ok) {
        fprintf(stderr, "velo-state: can't write %s\n", target);
        return 1;
    }
    return 0;
}

static const uint8_t *ce1_record(const store_t *store, uint32_t offset, int type, uint32_t minimum, uint32_t *size) {
    if (!offset || offset + 4 > store->registry_size) return NULL;
    const uint8_t *record = store->registry + offset;
    *size = get16(record);
    if (record[2] != type || *size < minimum || offset + *size > store->registry_size) return NULL;
    return record;
}

static bool read_key(const store_t *store, uint32_t id, uint32_t *sibling, uint32_t *child, uint32_t *value, char *name, size_t size) {
    static record_t record;
    if (!store->ce2) {
        uint32_t length;
        const uint8_t *key = ce1_record(store, id, CE1_KEY, 24, &length);
        if (!key) return false;
        *child = get32(key + 4);
        *sibling = get32(key + 8);
        *value = get32(key + 12);
        uint32_t bytes = key[20];
        if (24 + bytes > length) bytes = length - 24;
        utf16_to_utf8(key + 24, bytes / 2, name, size);
        return true;
    }
    if (!read_object(store, id, &record) || record.type != OBJECT_KEY || record.size < 10) return false;
    *sibling = get16(record.data + 2);
    *child = get16(record.data + 4);
    *value = get16(record.data + 6);
    uint32_t units = record.data[8];
    if (10 + 2 * units > record.size) units = (record.size - 10) / 2;
    utf16_to_utf8(record.data + 10, units, name, size);
    return true;
}

static bool ce1_root(const store_t *store, int index, uint32_t *first, uint32_t *value) {
    uint32_t offset = CE1_BLOCK_HEADER, size;
    while (offset + 4 <= CE1_BLOCK_SIZE && offset + 4 <= store->registry_size && (size = get16(store->registry + offset))) {
        const uint8_t *key = ce1_record(store, offset, CE1_KEY, 24, &size);
        if (key && key[19] == CE1_ROOT_MARKER && key[16] == index) {
            *first = get32(key + 4);
            *value = get32(key + 12);
            return true;
        }
        offset += size;
    }
    return false;
}

static bool registry_root(const store_t *store, const char *name, size_t length, uint32_t *first, uint32_t *value) {
    static const struct { const char *name; int index; } roots[] = {
        { "HKCR", 0 }, { "HKEY_CLASSES_ROOT", 0 }, { "HKCU", 1 }, { "HKEY_CURRENT_USER", 1 },
        { "HKLM", 2 }, { "HKEY_LOCAL_MACHINE", 2 }, { "HKU", 3 }, { "HKEY_USERS", 3 },
    };
    static record_t record;
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        if (strlen(roots[i].name) != length || strncasecmp(name, roots[i].name, length)) continue;
        *value = 0;
        if (!store->ce2) {
            if (store->registry && ce1_root(store, roots[i].index, first, value)) return true;
        } else if (read_object(store, store->registry_roots, &record) && record.type == OBJECT_ROOTS && 2u + 2 * (roots[i].index + 1) <= record.size) {
            *first = get16(record.data + 2 + 2 * roots[i].index);
            if (10u + 2 * (roots[i].index + 1) <= record.size) *value = get16(record.data + 10 + 2 * roots[i].index);
            return true;
        }
        fprintf(stderr, "velo-state: no %.*s in this state\n", (int)length, name);
        return false;
    }
    fprintf(stderr, "velo-state: key must start with HKCR, HKCU, HKLM or HKU: %.*s\n", (int)length, name);
    return false;
}

static bool open_key(const store_t *store, const char *path, uint32_t *child, uint32_t *value) {
    size_t root_length = strcspn(path, "/\\");
    if (!registry_root(store, path, root_length, child, value)) return false;
    const char *at = path + root_length;
    while (*at) {
        while (*at == '/' || *at == '\\') at++;
        if (!*at) break;
        size_t length = strcspn(at, "/\\");
        uint32_t id = *child, sibling, next_child, next_value;
        char name[NAME_MAX_UTF8];
        bool found = false;
        uint32_t steps = 0;
        while (id && steps++ < LIST_MAX && read_key(store, id, &sibling, &next_child, &next_value, name, sizeof name)) {
            if (strlen(name) == length && !strncasecmp(name, at, length)) {
                found = true;
                break;
            }
            id = sibling;
        }
        if (!found) {
            fprintf(stderr, "velo-state: no key %s\n", path);
            return false;
        }
        *child = next_child;
        *value = next_value;
        at += length;
    }
    return true;
}

static void format_value(uint32_t type, const uint8_t *data, uint32_t length, char *out, size_t size) {
    if (type == REG_DWORD && length >= 4) {
        uint32_t value = get32(data);
        snprintf(out, size, "dword %u (0x%x)", value, value);
    } else if (type == REG_SZ || type == REG_MULTI_SZ) {
        uint8_t copy[VALUE_DATA_MAX];
        char text[VALUE_DATA_MAX * 2];
        memcpy(copy, data, length);
        for (uint32_t i = 0; type == REG_MULTI_SZ && i + 3 < length; i += 2) {
            if (!copy[i] && !copy[i + 1] && (copy[i + 2] || copy[i + 3])) copy[i] = '|';
        }
        utf16_to_utf8(copy, length / 2, text, sizeof text);
        snprintf(out, size, "%s \"%s\"", type == REG_SZ ? "string" : "multi", text);
    } else {
        int used = snprintf(out, size, "type %u:", type);
        for (uint32_t i = 0; i < length && used + 4 < (int)size; i++) used += snprintf(out + used, size - (size_t)used, " %02x", data[i]);
    }
}

typedef bool (*value_fn)(void *context, const char *name, const char *formatted);

static bool emit_value(uint32_t type, const uint8_t *name, uint32_t units, const uint8_t *data, uint32_t length, value_fn emit, void *context) {
    if (length > VALUE_DATA_MAX) length = VALUE_DATA_MAX;
    char text[NAME_MAX_UTF8], formatted[VALUE_DATA_MAX * 3 + 32];
    utf16_to_utf8(name, units, text, sizeof text);
    format_value(type, data, length, formatted, sizeof formatted);
    return emit(context, *text ? text : "@", formatted);
}

static bool each_value(const store_t *store, uint32_t id, value_fn emit, void *context) {
    static record_t record;
    for (uint32_t steps = 0; id; steps++) {
        if (steps == LIST_MAX) return false;
        if (!store->ce2) {
            uint32_t size;
            const uint8_t *value = ce1_record(store, id, CE1_VALUE, 12, &size);
            if (!value || 12u + value[8] + get16(value + 10) > size) return false;
            if (!emit_value(value[9], value + 12, value[8] / 2u, value + 12 + value[8], get16(value + 10), emit, context)) return false;
            id = get32(value + 4);
            continue;
        }
        if (!read_object(store, id, &record) || record.type != OBJECT_VALUE || record.size < 10) return false;
        uint16_t next = get16(record.data + 2), type = get16(record.data + 4);
        uint32_t units = get16(record.data + 6), length = get16(record.data + 8);
        if (10 + 2 * units + length > record.size) return false;
        if (!emit_value(type, record.data + 10, units, record.data + 10 + 2 * units, length, emit, context)) return false;
        id = next;
    }
    return true;
}

typedef struct {
    const char *path;
    lines_t    *lines;
    const char *wanted;
    bool        found;
} value_context_t;

static bool add_line(lines_t *lines, const char *text) {
    if (lines->count == lines->capacity) {
        size_t capacity = lines->capacity ? lines->capacity * 2 : 1024;
        char **grown = realloc(lines->lines, capacity * sizeof *grown);
        if (!grown) return false;
        lines->lines = grown;
        lines->capacity = capacity;
    }
    lines->lines[lines->count] = strdup(text);
    return lines->lines[lines->count++] != NULL;
}

static bool print_value(void *context, const char *name, const char *formatted) {
    value_context_t *values = context;
    if (values->path) printf("%s\\%s = %s\n", values->path, name, formatted);
    else printf("%s = %s\n", name, formatted);
    return true;
}

static bool collect_value(void *context, const char *name, const char *formatted) {
    value_context_t *values = context;
    size_t size = strlen(values->path) + strlen(name) + strlen(formatted) + 8;
    char *line = malloc(size);
    if (!line) return false;
    snprintf(line, size, "%s\\%s = %s", values->path, name, formatted);
    bool ok = add_line(values->lines, line);
    free(line);
    return ok;
}

static bool match_value(void *context, const char *name, const char *formatted) {
    value_context_t *values = context;
    if (!values->found && !strcasecmp(name, values->wanted)) {
        printf("%s\n", formatted);
        values->found = true;
    }
    return true;
}

static bool walk_key(const store_t *store, uint32_t child, uint32_t value, const char *path, bool recursive, lines_t *lines, int depth) {
    if (depth > DEPTH_MAX) return false;
    value_context_t values = { .path = recursive ? path : NULL, .lines = lines };
    if (!each_value(store, value, lines ? collect_value : print_value, &values)) return false;
    for (uint32_t id = child, steps = 0; id; steps++) {
        if (steps == LIST_MAX) return false;
        uint32_t sibling, grandchild, child_value;
        char name[NAME_MAX_UTF8];
        if (!read_key(store, id, &sibling, &grandchild, &child_value, name, sizeof name)) return false;
        if (!recursive) printf("%s\\\n", name);
        else {
            size_t size = strlen(path) + strlen(name) + 2;
            char *child_path = malloc(size);
            if (!child_path) return false;
            snprintf(child_path, size, "%s\\%s", path, name);
            bool ok = walk_key(store, grandchild, child_value, child_path, true, lines, depth + 1);
            free(child_path);
            if (!ok) return false;
        }
        id = sibling;
    }
    return true;
}

static int registry(const store_t *store, int count, char **args) {
    if (count < 2) return 2;
    const char *action = args[0];
    if (!(!strcmp(action, "ls") && count == 2) && !(!strcmp(action, "dump") && count == 2) && !(!strcmp(action, "get") && count == 3)) return 2;
    char path[1100];
    snprintf(path, sizeof path, "%s", args[1]);
    for (char *p = path; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    size_t length = strlen(path);
    while (length && path[length - 1] == '\\') path[--length] = 0;
    uint32_t child, value;
    if (!open_key(store, path, &child, &value)) return 1;
    if (!strcmp(action, "get")) {
        value_context_t values = { .wanted = args[2] };
        if (!each_value(store, value, match_value, &values)) {
            fprintf(stderr, "velo-state: %s is damaged\n", path);
            return 1;
        }
        if (!values.found) {
            fprintf(stderr, "velo-state: no value %s in %s\n", args[2], path);
            return 1;
        }
        return 0;
    }
    if (!walk_key(store, child, value, path, !strcmp(action, "dump"), NULL, 0)) {
        fprintf(stderr, "velo-state: %s is damaged\n", path);
        return 1;
    }
    return 0;
}

static bool collect_files(const store_t *store, uint16_t folder, const char *path, lines_t *lines, int depth) {
    node_t parent, node;
    if (depth > DEPTH_MAX || !read_node(store, folder, &parent)) return false;
    uint32_t steps = 0;
    for (uint16_t id = parent.child; id; id = node.sibling) {
        if (steps++ == LIST_MAX) return false;
        if (!read_node(store, id, &node)) return false;
        size_t size = strlen(path) + strlen(node.name) + 64;
        char *line = malloc(size);
        if (!line) return false;
        snprintf(line, size, "%s\\%s", path, node.name);
        bool ok;
        if (node.type == OBJECT_FOLDER) {
            char *child_path = strdup(line);
            snprintf(line, size, "%s\\%s\\", path, node.name);
            ok = child_path && add_line(lines, line) && collect_files(store, node.id, child_path, lines, depth + 1);
            free(child_path);
        } else {
            uint32_t length;
            uint8_t *data = read_file(store, &node, &length);
            size_t used = strlen(line);
            if (data) snprintf(line + used, size - used, " = %u bytes, crc %08lx", length, crc32(0, data, length));
            else snprintf(line + used, size - used, " = damaged");
            free(data);
            ok = add_line(lines, line);
        }
        free(line);
        if (!ok) return false;
    }
    return true;
}

static bool collect(const store_t *store, lines_t *lines) {
    static const char *roots[] = { "HKCR", "HKCU", "HKLM", "HKU" };
    for (int i = 0; i < (store->ce2 ? 3 : 4); i++) {
        uint32_t child, value;
        if (!open_key(store, roots[i], &child, &value) || !walk_key(store, child, value, roots[i], true, lines, 0)) return false;
    }
    return collect_files(store, 0, "", lines, 0);
}

static int compare_lines(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int diff(const store_t *store, const char *other_path) {
    store_t *other = malloc(sizeof *other);
    if (!other || !open_state(other_path, other)) return 1;
    lines_t before = { 0 }, after = { 0 };
    if (!collect(store, &before) || !collect(other, &after)) {
        fprintf(stderr, "velo-state: an object store is damaged\n");
        return 1;
    }
    qsort(before.lines, before.count, sizeof *before.lines, compare_lines);
    qsort(after.lines, after.count, sizeof *after.lines, compare_lines);
    size_t i = 0, j = 0;
    while (i < before.count || j < after.count) {
        int order = i == before.count ? 1 : j == after.count ? -1 : strcmp(before.lines[i], after.lines[j]);
        if (order < 0) printf("- %s\n", before.lines[i++]);
        else if (order > 0) printf("+ %s\n", after.lines[j++]);
        else {
            i++;
            j++;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        fputs(usage, stdout);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        const char *slash = strrchr(argv[0], '/');
        printf("%s %s\n", slash ? slash + 1 : argv[0], options_version());
        return 0;
    }
    if (argc > 1 && argv[1][0] == '-') {
        fprintf(stderr, "velo-state: unknown option %s (see --help)\n", argv[1]);
        return 2;
    }
    if (argc < 3) {
        fputs(usage, stderr);
        return 2;
    }
    static store_t store;
    if (!open_state(argv[1], &store)) return 1;
    const char *command = argv[2];
    int count = argc - 3;
    char **args = argv + 3;
    int status = 2;
    if (!strcmp(command, "ls") && count <= 1) status = list(&store, count ? args[0] : "");
    else if (!strcmp(command, "get") && (count == 1 || count == 2)) status = get(&store, args[0], count == 2 ? args[1] : NULL);
    else if (!strcmp(command, "reg")) status = registry(&store, count, args);
    else if (!strcmp(command, "diff") && count == 1) status = diff(&store, args[0]);
    if (status == 2) fputs(usage, stderr);
    return status;
}

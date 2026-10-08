#include "core/screen.h"

#include "native/lzw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const screen_size_t SCREEN_PRESETS[] = {
    { 480, 240 },
    { 640, 240 },
    { 640, 480 },
    { 800, 600 },
};
const int SCREEN_PRESET_COUNT = (int)(sizeof SCREEN_PRESETS / sizeof SCREEN_PRESETS[0]);

#define PLAN_MAX       64
#define KSEG_PA_MASK   0x1FFFFFFFu

enum { AT = 1, V0 = 2, A0 = 4, T1 = 9, T3 = 11, T4 = 12, T5 = 13, T6 = 14, T7 = 15, T8 = 24, T9 = 25 };

static uint32_t lui(int rt, uint32_t imm) {
    return 0x3C000000u | (uint32_t)rt << 16 | (imm & 0xFFFF);
}
static uint32_t ori(int rt, int rs, uint32_t imm) {
    return 0x34000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | (imm & 0xFFFF);
}
static uint32_t addiu(int rt, int rs, uint32_t imm) {
    return 0x24000000u | (uint32_t)rs << 21 | (uint32_t)rt << 16 | (imm & 0xFFFF);
}
static uint32_t sw(int rt, int32_t offset, int base) {
    return 0xAC000000u | (uint32_t)base << 21 | (uint32_t)rt << 16 | ((uint32_t)offset & 0xFFFF);
}
static uint32_t lw(int rt, int32_t offset, int base) {
    return 0x8C000000u | (uint32_t)base << 21 | (uint32_t)rt << 16 | ((uint32_t)offset & 0xFFFF);
}
static uint32_t multu(int rs, int rt) {
    return (uint32_t)rs << 21 | (uint32_t)rt << 16 | 0x19;
}
static uint32_t mflo(int rd) {
    return (uint32_t)rd << 11 | 0x12;
}

typedef struct {
    uint32_t pa, length;
    uint8_t bytes[4];
    uint8_t *block;
} planned_t;

typedef struct {
    screen_rom_t *roms;
    int rom_count;
    planned_t writes[PLAN_MAX];
    int count;
    bool failed;
} plan_t;

static uint8_t *rom_bytes(const plan_t *plan, uint32_t va, uint32_t length) {
    uint32_t pa = va & KSEG_PA_MASK;
    for (int i = 0; i < plan->rom_count; i++) {
        const screen_rom_t *rom = &plan->roms[i];
        if (pa >= rom->pa && length <= rom->size && pa - rom->pa <= rom->size - length) return rom->data + (pa - rom->pa);
    }
    return NULL;
}

static bool read_word(const plan_t *plan, uint32_t va, uint32_t *value) {
    const uint8_t *at = rom_bytes(plan, va, 4);
    if (!at) return false;
    *value = (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24;
    return true;
}

static bool word_is(const plan_t *plan, uint32_t va, uint32_t expected) {
    uint32_t value;
    return read_word(plan, va, &value) && value == expected;
}

static void plan_word(plan_t *plan, uint32_t va, uint32_t expected, uint32_t value) {
    if (plan->failed) return;
    if (plan->count == PLAN_MAX || !word_is(plan, va, expected)) {
        plan->failed = true;
        return;
    }
    planned_t *write = &plan->writes[plan->count++];
    write->pa = va & KSEG_PA_MASK;
    write->length = 4;
    write->block = NULL;
    for (int b = 0; b < 4; b++) write->bytes[b] = (uint8_t)(value >> (8 * b));
}

static void plan_block(plan_t *plan, uint32_t va, uint8_t *block, uint32_t length) {
    if (plan->failed || plan->count == PLAN_MAX || !rom_bytes(plan, va, length)) {
        free(block);
        plan->failed = true;
        return;
    }
    planned_t *write = &plan->writes[plan->count++];
    write->pa = va & KSEG_PA_MASK;
    write->length = length;
    write->block = block;
}

static void plan_free(plan_t *plan) {
    for (int i = 0; i < plan->count; i++) free(plan->writes[i].block);
    plan->count = 0;
}

static uint32_t page_round(uint32_t bytes) {
    return (bytes + 0xFFF) & ~0xFFFu;
}





#define CE1_GWES_DATA       0x9F454C10u
#define CE1_GWES_DATA_SIZE  0x28Fu
#define CE1_GWES_DATA_VSIZE 0x1588u
#define CE1_ROMHDR_RAM_FREE 0x9FAACF6Cu
#define CE1_FRAMEBUFFER     0x20000u

static void plan_ce1(plan_t *plan, screen_size_t size) {
    uint32_t bytes = (uint32_t)size.width * size.height * 2 / 8;
    uint32_t vidrate = (0x085 + 1) * SCREEN_STOCK_HEIGHT / size.height - 1;
    uint32_t ctl2 = vidrate << 22 | (uint32_t)(size.width / 4 - 1) << 12 | (uint32_t)(size.height - 1);
    uint32_t end = (CE1_FRAMEBUFFER + bytes) & 0xFFFF0u;
    plan_word(plan, 0x9F4234E8u, lui(T4, 0x2147), lui(T4, ctl2 >> 16));
    plan_word(plan, 0x9F4234F0u, ori(T4, T4, 0x70EF), ori(T4, T4, ctl2));
    plan_word(plan, 0x9F4234F8u, lui(T6, 0x7700), lui(T6, 0x7700 | end >> 16));
    plan_word(plan, 0x9F4234FCu, addiu(T5, 0, 0x2000), lui(T5, CE1_FRAMEBUFFER >> 16));
    plan_word(plan, 0x9F423504u, ori(T6, T6, 0x9080), ori(T6, T6, end));
    plan_word(plan, CE1_ROMHDR_RAM_FREE, 0x80020000u, 0x80000000u | page_round(CE1_FRAMEBUFFER + bytes));
    if (plan->failed) return;
    const uint8_t *packed = rom_bytes(plan, CE1_GWES_DATA, CE1_GWES_DATA_SIZE);
    uint8_t *data = calloc(1, CE1_GWES_DATA_VSIZE), *repacked = malloc(CE1_GWES_DATA_SIZE);
    size_t length = packed && data && repacked ? lzw_decode(packed, CE1_GWES_DATA_SIZE, data, CE1_GWES_DATA_VSIZE) : 0;
    static const uint32_t stock[] = { 0xA0002000u, 480, 240 };
    bool matches = length >= 0x408 && !memcmp(data + 0x3E8, stock, sizeof stock) && data[0x404] == 30;
    if (matches) {
        uint32_t display[] = { 0xA0000000u | CE1_FRAMEBUFFER, size.width, size.height };
        memcpy(data + 0x3E8, display, sizeof display);
        uint32_t stride = (uint32_t)size.width * 2 / 32;
        memcpy(data + 0x404, &stride, sizeof stride);
    }
    size_t repacked_length = matches ? lzw_encode(data, length, 1, repacked, CE1_GWES_DATA_SIZE, false) : 0;
    free(data);
    if (!repacked_length) {
        free(repacked);
        plan->failed = true;
        return;
    }
    plan_block(plan, CE1_GWES_DATA, repacked, CE1_GWES_DATA_SIZE);
}

#define CE2_ROMHDR_POINTER 0x90005A98u
#define CE2_FRAMEBUFFER    0x40000u
#define CE2_DDI_TEXT       0x90079000u
#define CE2_DDI_TEXT_REAL  0x03FD1000u

static uint32_t stock_ddi(uint32_t real) {
    return CE2_DDI_TEXT + real - CE2_DDI_TEXT_REAL;
}

static bool read_string(const plan_t *plan, uint32_t va, char *out, size_t size) {
    for (size_t i = 0; i < size; i++) {
        const uint8_t *at = rom_bytes(plan, va + (uint32_t)i, 1);
        if (!at) return false;
        out[i] = (char)*at;
        if (!*at) return true;
    }
    return false;
}

static uint32_t find_rom_file(const plan_t *plan, uint32_t romhdr, const char *wanted) {
    uint32_t modules, files;
    if (!read_word(plan, romhdr + 16, &modules) || !read_word(plan, romhdr + 48, &files) || modules > 1024 || files > 4096) return 0;
    uint32_t table = romhdr + 84 + 32 * modules;
    for (uint32_t i = 0; i < files; i++) {
        uint32_t name_va, load;
        char name[64];
        if (!read_word(plan, table + 28 * i + 20, &name_va) || !read_word(plan, table + 28 * i + 24, &load)) return 0;
        if (read_string(plan, name_va, name, sizeof name) && !strcmp(name, wanted)) return load;
    }
    return 0;
}

static void plan_stock_ddi(plan_t *plan, screen_size_t size) {
    if (size.height != 240) {
        plan->failed = true;
        return;
    }
    enum { FRAMEBUFFER = -0x6FE8, WIDTH = -0x6FE4, HEIGHT = -0x6FE0, STRIDE = -0x6FCC, BOOT = -0x6FC8 };
    uint32_t start = stock_ddi(0x3FD15B0u), end = stock_ddi(0x3FD161Cu);
    if (!word_is(plan, stock_ddi(0x3FD15BCu), addiu(T7, 0, 0x78)) || !word_is(plan, stock_ddi(0x3FD15E4u), addiu(T9, 0, 0xF0)) ||
        !word_is(plan, stock_ddi(0x3FD1610u), addiu(T1, 0, 0x1E0))) {
        plan->failed = true;
        return;
    }
    const uint32_t body[] = {
        lw(A0, BOOT, AT),
        lw(T6, 0x5C, A0),
        ori(T7, 0, size.width / 8),
        multu(T6, T7),
        mflo(T7),
        sw(T7, STRIDE, AT),
        ori(T8, 0, size.width),
        sw(T8, WIDTH, AT),
        ori(T8, 0, size.height),
        sw(T8, HEIGHT, AT),
        lui(T8, 0xA000 | CE2_FRAMEBUFFER >> 16),
        ori(T8, T8, CE2_FRAMEBUFFER),
        sw(T8, FRAMEBUFFER, AT),
    };
    int words = (int)(sizeof body / sizeof body[0]);
    for (uint32_t va = start, k = 0; va < end; va += 4, k++) {
        uint32_t original;
        if (!read_word(plan, va, &original)) {
            plan->failed = true;
            return;
        }
        plan_word(plan, va, original, (int)k < words ? body[k] : 0);
    }
}

static void plan_upgrade_ddi(plan_t *plan, uint32_t image, screen_size_t size) {
    uint32_t pe_offset, image_base, sections_offset;
    if (!read_word(plan, image + 0x3C, &pe_offset) || !word_is(plan, image + pe_offset, 0x00004550u)) {
        plan->failed = true;
        return;
    }
    uint32_t pe = image + pe_offset, optional_size;
    if (!read_word(plan, pe + 20, &optional_size) || !read_word(plan, pe + 24 + 28, &image_base)) {
        plan->failed = true;
        return;
    }
    sections_offset = pe + 24 + (optional_size & 0xFFFF);
    uint32_t text_rva, text_raw, data_raw;
    if (!read_word(plan, sections_offset + 12, &text_rva) || !read_word(plan, sections_offset + 20, &text_raw) ||
        !read_word(plan, sections_offset + 40 + 20, &data_raw)) {
        plan->failed = true;
        return;
    }
    uint32_t block = image + data_raw + 0x18;
    plan_word(plan, block, 0xA000F000u, 0xA0000000u | CE2_FRAMEBUFFER);
    plan_word(plan, block + 4, 480, size.width);
    plan_word(plan, block + 8, 240, size.height);
    uint32_t text = image + text_raw - image_base - text_rva;
    plan_word(plan, text + 0x100015D4u, addiu(T9, 0, 0x78), addiu(T9, 0, size.width / 4));
    plan_word(plan, text + 0x100015FCu, addiu(T1, 0, 0xF0), addiu(T1, 0, size.width / 2));
    plan_word(plan, text + 0x10001628u, addiu(T3, 0, 0x1E0), addiu(T3, 0, size.width));
}

static void plan_ce2(plan_t *plan, screen_size_t size) {
    if (size.width > 640) {
        plan->failed = true;
        return;
    }
    uint32_t horizontal = (uint32_t)(size.width / 4 - 1) << 12, pixels = (uint32_t)size.width * size.height;
    uint32_t bytes = pixels * 4 / 8, romhdr;
    plan_word(plan, 0x9005D32Cu, lui(AT, 7), lui(AT, horizontal >> 16));
    plan_word(plan, 0x9005D330u, ori(AT, AT, 0x7000), ori(AT, AT, horizontal));
    plan_word(plan, 0x9005D340u, ori(T7, T9, 0xEF), ori(T7, T9, size.height - 1u));
    uint32_t clock = 0x8CA000u * SCREEN_STOCK_HEIGHT / size.height, upgrade_clock = 0x2328000u * SCREEN_STOCK_HEIGHT / size.height;
    plan_word(plan, 0x9005D2D0u, lui(A0, 0x8C), lui(A0, clock >> 16));
    plan_word(plan, 0x9005D2D8u, ori(A0, A0, 0xA000), ori(A0, A0, clock));
    plan_word(plan, 0x9005D2DCu, lui(A0, 0x232), lui(A0, upgrade_clock >> 16));
    plan_word(plan, 0x9005D2E0u, ori(A0, A0, 0x8000), ori(A0, A0, upgrade_clock));
    plan_word(plan, 0x9005D368u, ori(V0, 0, 0xF000), lui(V0, CE2_FRAMEBUFFER >> 16));
    plan_word(plan, 0x9005D374u, lui(AT, 1), lui(AT, pixels >> 16));
    plan_word(plan, 0x9005D378u, ori(AT, AT, 0xC200), ori(AT, AT, pixels));
    if (plan->failed || !read_word(plan, CE2_ROMHDR_POINTER, &romhdr)) {
        plan->failed = true;
        return;
    }
    plan_word(plan, romhdr + 24, 0x80037000u, 0x80000000u | page_round(CE2_FRAMEBUFFER + bytes));
    uint32_t upgrade_ddi = find_rom_file(plan, romhdr, "ddi.dll");
    if (upgrade_ddi) plan_upgrade_ddi(plan, upgrade_ddi, size);
    else plan_stock_ddi(plan, size);
}

int screen_preset_index(screen_size_t size) {
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (SCREEN_PRESETS[i].width == size.width && SCREEN_PRESETS[i].height == size.height) return i;
    }
    return -1;
}

bool screen_parse(const char *text, screen_size_t *size) {
    unsigned width, height;
    char tail;
    if (sscanf(text, "%ux%u%c", &width, &height, &tail) != 2 || width > 0xFFFF || height > 0xFFFF) return false;
    screen_size_t parsed = { (uint16_t)width, (uint16_t)height };
    if (screen_preset_index(parsed) < 0) return false;
    *size = parsed;
    return true;
}

bool screen_rom_patch(screen_rom_t *roms, int rom_count, screen_size_t size, screen_patch_t *patch) {
    patch->edits = NULL;
    patch->count = 0;
    if (size.width == SCREEN_STOCK_WIDTH && size.height == SCREEN_STOCK_HEIGHT) return true;
    if (screen_preset_index(size) < 0) return false;
    plan_t plan = { roms, rom_count, { { 0 } }, 0, false };
    if (word_is(&plan, 0x9F4234E8u, lui(T4, 0x2147))) plan_ce1(&plan, size);
    else if (word_is(&plan, 0x9005D32Cu, lui(AT, 7))) plan_ce2(&plan, size);
    else plan.failed = true;
    if (plan.failed) {
        plan_free(&plan);
        return false;
    }
    patch->edits = calloc((size_t)plan.count, sizeof *patch->edits);
    for (int i = 0; i < plan.count && patch->edits; i++) {
        planned_t *write = &plan.writes[i];
        uint8_t *at = rom_bytes(&plan, write->pa, write->length);
        screen_edit_t *edit = &patch->edits[patch->count];
        edit->original = malloc(write->length);
        if (!edit->original) break;
        edit->pa = write->pa;
        edit->length = write->length;
        memcpy(edit->original, at, write->length);
        memcpy(at, write->block ? write->block : write->bytes, write->length);
        patch->count++;
    }
    bool complete = patch->edits && patch->count == plan.count;
    plan_free(&plan);
    if (!complete) screen_rom_revert(roms, rom_count, patch);
    return complete;
}

void screen_rom_revert(screen_rom_t *roms, int rom_count, screen_patch_t *patch) {
    plan_t plan = { roms, rom_count, { { 0 } }, 0, false };
    for (int i = patch->count - 1; i >= 0; i--) {
        screen_edit_t *edit = &patch->edits[i];
        uint8_t *at = rom_bytes(&plan, edit->pa, edit->length);
        if (at) memcpy(at, edit->original, edit->length);
        free(edit->original);
    }
    free(patch->edits);
    patch->edits = NULL;
    patch->count = 0;
}

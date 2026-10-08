#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SCREEN_STOCK_WIDTH  480
#define SCREEN_STOCK_HEIGHT 240
#define SCREEN_MAX_WIDTH    800
#define SCREEN_MAX_HEIGHT   600

typedef struct {
    uint16_t width, height;
} screen_size_t;

extern const screen_size_t SCREEN_PRESETS[];
extern const int SCREEN_PRESET_COUNT;

typedef struct {
    uint8_t *data;
    uint32_t pa, size;
} screen_rom_t;

typedef struct {
    uint32_t pa, length;
    uint8_t *original;
} screen_edit_t;

typedef struct {
    screen_edit_t *edits;
    int count;
} screen_patch_t;

int  screen_preset_index(screen_size_t size);
bool screen_parse(const char *text, screen_size_t *size);
bool screen_rom_patch(screen_rom_t *roms, int rom_count, screen_size_t size, screen_patch_t *patch);
void screen_rom_revert(screen_rom_t *roms, int rom_count, screen_patch_t *patch);

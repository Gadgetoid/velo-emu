#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/screen.h"

extern const int DIALOG_MEMORY_COUNT;
extern const uint32_t DIALOG_MEMORY_SIZES[];
extern const char *const DIALOG_MEMORY_LABELS[];
extern const char *const DIALOG_NEW_MACHINE_MESSAGE;
extern const char *const DIALOG_MANAGE_MESSAGE;
extern const char *const DIALOG_CLOCK_LABEL;
extern const char *const DIALOG_ROM_PROMPT;
extern const char *const DIALOG_NOT_A_ROM;
extern const char *const DIALOG_NO_ROM_HINT;

typedef struct {
    char path[1024];
    char label[160];
    uint32_t screens;
} dialog_rom_t;

typedef uint32_t (*dialog_probe_fn)(const char *path, char *label, size_t label_size);

typedef struct {
    char name[96];
    char rom[1024];
    screen_size_t screen;
    uint32_t memory;
    bool host_time;
} dialog_machine_t;

const char *dialog_screen_label(int preset);
bool        dialog_rom_allows_screen(const dialog_rom_t *rom, int preset);

typedef enum { DIALOG_MANAGE_CLOSE, DIALOG_MANAGE_RESET, DIALOG_MANAGE_DELETE } dialog_manage_t;

bool            dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result);
dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen);

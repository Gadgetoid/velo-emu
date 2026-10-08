#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/screen.h"

#define DIALOG_MEMORY_COUNT 5

extern const uint32_t DIALOG_MEMORY_SIZES[DIALOG_MEMORY_COUNT];

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

typedef enum { DIALOG_MANAGE_CLOSE, DIALOG_MANAGE_RESET, DIALOG_MANAGE_DELETE } dialog_manage_t;

bool            dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result);
dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen);

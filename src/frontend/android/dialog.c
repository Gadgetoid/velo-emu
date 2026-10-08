#include "frontend/common/dialog.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "frontend/android/android.h"
#include "frontend/android/list.h"
#include "frontend/android/text.h"

#define STORAGE_ROOT   "/storage/emulated/0"
#define PROGRESS_WIDTH 0.6f
#define NAME_MAX_BYTES 96

enum { CHOICE_ROM = 0x1000, CHOICE_MEMORY = 0x2000, CHOICE_SCREEN = 0x3000, CHOICE_CLOCK = 0x4000, CHOICE_MACHINE = 0x5000, CHOICE_FOLDER = 0x6000 };

static SDL_Window *app_window(void) {
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    SDL_Window *window = windows && count ? windows[0] : NULL;
    SDL_free(windows);
    return window;
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    (void)probe;
    if (rom_count <= 0) return false;
    int rom = 0;
    for (int i = 0; i < rom_count; i++) {
        if (!strcmp(roms[i].path, result->rom)) rom = i;
    }
    int screen = screen_preset_index(result->screen);
    if (screen < 0 || !dialog_rom_allows_screen(&roms[rom], screen)) screen = 0;
    uint32_t memory = result->memory;
    bool host_time = result->host_time;
    list_scroll_t scroll = { 0 };
    for (;;) {
        list_t list = { { "Cancel", "Create" }, 2, -1, { { 0 } }, 0 };
        list_add_row(&list, ROW_HEADING, 0, "ROM", false, false);
        for (int i = 0; i < rom_count; i++) list_add_row(&list, ROW_ITEM, CHOICE_ROM + i, roms[i].label, i == rom, false);
        list_add_row(&list, ROW_HEADING, 0, "Memory", false, false);
        for (int i = 0; i < DIALOG_MEMORY_COUNT; i++) list_add_row(&list, ROW_ITEM, CHOICE_MEMORY + i, DIALOG_MEMORY_LABELS[i], DIALOG_MEMORY_SIZES[i] == memory, false);
        list_add_row(&list, ROW_HEADING, 0, "Screen", false, false);
        for (int i = 0; i < SCREEN_PRESET_COUNT && dialog_screen_label(i); i++) {
            if (dialog_rom_allows_screen(&roms[rom], i)) list_add_row(&list, ROW_ITEM, CHOICE_SCREEN + i, dialog_screen_label(i), i == screen, false);
        }
        list_add_row(&list, ROW_HEADING, 0, "Clock", false, false);
        list_add_row(&list, ROW_ITEM, CHOICE_CLOCK, DIALOG_CLOCK_LABEL, host_time, false);
        tap_t tap = list_modal_step(&scroll, &list, window, 0);
        if (!tap.tapped) continue;
        if (tap.tab == 0) return false;
        if (tap.tab == 1) {
            result->name[0] = 0;
            SDL_strlcpy(result->rom, roms[rom].path, sizeof result->rom);
            result->screen = SCREEN_PRESETS[screen];
            result->memory = memory;
            result->host_time = host_time;
            return true;
        }
        if (tap.tag >= CHOICE_ROM && tap.tag < CHOICE_ROM + rom_count) {
            rom = tap.tag - CHOICE_ROM;
            if (!dialog_rom_allows_screen(&roms[rom], screen)) screen = 0;
        } else if (tap.tag >= CHOICE_MEMORY && tap.tag < CHOICE_MEMORY + DIALOG_MEMORY_COUNT) {
            memory = DIALOG_MEMORY_SIZES[tap.tag - CHOICE_MEMORY];
        } else if (tap.tag >= CHOICE_SCREEN && tap.tag < CHOICE_SCREEN + SCREEN_PRESET_COUNT) {
            screen = tap.tag - CHOICE_SCREEN;
        } else if (tap.tag == CHOICE_CLOCK) {
            host_time = !host_time;
        }
    }
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    (void)current;
    int selected = *chosen >= 0 && *chosen < count ? *chosen : 0;
    list_scroll_t scroll = { 0 };
    for (;;) {
        list_t list = { { "Close", "Reset", "Delete" }, 3, -1, { { 0 } }, 0 };
        list_add_row(&list, ROW_HEADING, 0, "Machines", false, false);
        for (int i = 0; i < count; i++) list_add_row(&list, ROW_ITEM, CHOICE_MACHINE + i, names[i], i == selected, false);
        tap_t tap = list_modal_step(&scroll, &list, window, 0);
        if (!tap.tapped) continue;
        if (tap.tab == 0) return DIALOG_MANAGE_CLOSE;
        if (tap.tab == 1 || tap.tab == 2) {
            *chosen = selected;
            return tap.tab == 1 ? DIALOG_MANAGE_RESET : DIALOG_MANAGE_DELETE;
        }
        if (tap.tag >= CHOICE_MACHINE && tap.tag < CHOICE_MACHINE + count) selected = tap.tag - CHOICE_MACHINE;
    }
}

void android_progress(const char *title, float fraction) {
    SDL_Window *window = app_window();
    SDL_Renderer *renderer = window ? SDL_GetRenderer(window) : NULL;
    if (!renderer) return;
    SDL_PumpEvents();
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    float scale = text_display_scale(window);
    float size = floorf(LIST_ROW_POINTS * scale * LIST_LABEL_HEIGHT);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderClear(renderer);
    float bar_w = floorf(width * PROGRESS_WIDTH), bar_h = floorf(8 * scale);
    float x = floorf((width - bar_w) / 2), y = floorf(height / 2.0f);
    SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
    text_draw(renderer, floorf((width - text_width(renderer, title, size)) / 2), y - size - floorf(12 * scale), title, size);
    SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ x, y, bar_w, bar_h });
    SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
    if (fraction >= 0) {
        SDL_RenderFillRect(renderer, &(SDL_FRect){ x, y, floorf(bar_w * fminf(fraction, 1)), bar_h });
    } else {
        float segment = floorf(bar_w / 4), phase = (float)(SDL_GetTicks() % 1500) / 1500.0f;
        SDL_RenderFillRect(renderer, &(SDL_FRect){ x + floorf((bar_w - segment) * phase), y, segment, bar_h });
    }
    SDL_RenderPresent(renderer);
}

static int compare_names(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static int list_folders(const char *path, char names[][NAME_MAX_BYTES], int max) {
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (entry->d_name[0] == '.') continue;
        char full[1300];
        struct stat info;
        if (snprintf(full, sizeof full, "%s/%s", path, entry->d_name) >= (int)sizeof full || stat(full, &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        SDL_strlcpy(names[count++], entry->d_name, NAME_MAX_BYTES);
    }
    closedir(dir);
    qsort(names, (size_t)count, NAME_MAX_BYTES, compare_names);
    return count;
}

bool android_choose_folder(const char *title, const char *start, char *path, size_t size) {
    enum { TAB_CANCEL, TAB_UP, TAB_CHOOSE };
    SDL_Window *window = app_window();
    char (*names)[NAME_MAX_BYTES] = calloc(LIST_ROW_MAX, NAME_MAX_BYTES);
    if (!window || !names) {
        free(names);
        return false;
    }
    char current[1024], shown[1100];
    struct stat info;
    SDL_strlcpy(current, start && stat(start, &info) == 0 && S_ISDIR(info.st_mode) ? start : STORAGE_ROOT, sizeof current);
    list_scroll_t scroll = { 0 };
    bool chosen = false;
    for (;;) {
        list_t list = { { "Cancel", "Up", "Choose" }, 3, -1, { { 0 } }, 0 };
        bool at_root = !strcmp(current, STORAGE_ROOT);
        if (!strncmp(current, STORAGE_ROOT, strlen(STORAGE_ROOT))) snprintf(shown, sizeof shown, "Phone%s", current + strlen(STORAGE_ROOT));
        else SDL_strlcpy(shown, current, sizeof shown);
        list_add_row(&list, ROW_HEADING, 0, title, false, false);
        list_add_row(&list, ROW_ITEM, -1, shown, true, true);
        int count = list_folders(current, names, LIST_ROW_MAX - 3);
        for (int i = 0; i < count; i++) list_add_row(&list, ROW_ITEM, CHOICE_FOLDER + i, names[i], false, false);
        tap_t tap = list_modal_step(&scroll, &list, window, TAB_CANCEL);
        if (!tap.tapped) continue;
        if (tap.tab == TAB_CANCEL) break;
        if (tap.tab == TAB_CHOOSE) {
            SDL_strlcpy(path, current, size);
            chosen = true;
            break;
        }
        if (tap.tab == TAB_UP && !at_root) {
            char *slash = strrchr(current, '/');
            if (slash && slash != current) *slash = 0;
            list_reset(&scroll);
        } else if (tap.tag >= CHOICE_FOLDER && tap.tag < CHOICE_FOLDER + count) {
            size_t length = strlen(current);
            snprintf(current + length, sizeof current - length, "/%s", names[tap.tag - CHOICE_FOLDER]);
            list_reset(&scroll);
        }
    }
    free(names);
    return chosen;
}

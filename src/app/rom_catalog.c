#include "app/rom_catalog.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "core/machine.h"
#include "core/screen.h"
#include "util/file.h"
#include "util/marker.h"

#define ROM_MIN_BYTES   (1024 * 1024)
#define ROM_MAX_BYTES   (64 * 1024 * 1024)
#define ROM_PROBE_CACHE 32

enum { ROM_UNMARKED, ROM_PATCHED, ROM_PATCHED_115K };

typedef struct {
    char path[1024];
    off_t size;
    time_t modified;
    int system;
    uint32_t screens;
    int rank;
} rom_probe_t;

static rom_probe_t rom_probes[ROM_PROBE_CACHE];
static int rom_probe_count = 0;
static int rom_probe_next = 0;

static int inspect_rom(const char *path, uint32_t *screens, int *rank) {
    *screens = 0;
    *rank = ROM_UNMARKED;
    size_t size;
    uint8_t *rom = file_read(path, &size);
    if (!rom) return 0;
    char sets[512], os[16];
    int marked = ROM_UNMARKED, marked_system = 0;
    if (marker_value(rom, size, "patch_sets", sets, sizeof sets)) {
        marked = marker_has_set(sets, "pc-link-115k") ? ROM_PATCHED_115K : ROM_PATCHED;
        marked_system = !marker_value(rom, size, "os", os, sizeof os) || !strcmp(os, "ce2") ? 2 : !strcmp(os, "ce1") ? 1 : 0;
    }
    char error[256];
    machine_t *machine = machine_create(rom, size, error, sizeof error);
    free(rom);
    if (!machine) return 0;
    int system = machine_rom_system(machine);
    if (system == marked_system) *rank = marked;
    for (int i = 0; i < SCREEN_PRESET_COUNT; i++) {
        if (machine_screen_supported(machine, SCREEN_PRESETS[i])) *screens |= 1u << i;
    }
    machine_destroy(machine);
    return system;
}

static int cached_rom_system(const char *path, const struct stat *info, uint32_t *screens, int *rank) {
    rom_probe_t *probe = NULL;
    for (int i = 0; i < rom_probe_count && !probe; i++) {
        if (!strcmp(rom_probes[i].path, path)) probe = &rom_probes[i];
    }
    if (probe && probe->size == info->st_size && probe->modified == info->st_mtime) {
        *screens = probe->screens;
        *rank = probe->rank;
        return probe->system;
    }
    if (!probe) {
        if (rom_probe_count < ROM_PROBE_CACHE) {
            probe = &rom_probes[rom_probe_count++];
        } else {
            probe = &rom_probes[rom_probe_next];
            rom_probe_next = (rom_probe_next + 1) % ROM_PROBE_CACHE;
        }
        snprintf(probe->path, sizeof probe->path, "%s", path);
    }
    probe->size = info->st_size;
    probe->modified = info->st_mtime;
    probe->system = inspect_rom(path, &probe->screens, &probe->rank);
    *screens = probe->screens;
    *rank = probe->rank;
    return probe->system;
}

static int probe_rom(const char *path, const struct stat *info, uint32_t *screens, int *rank) {
    if (!S_ISREG(info->st_mode) || info->st_size < ROM_MIN_BYTES || info->st_size > ROM_MAX_BYTES) {
        *screens = 0;
        *rank = ROM_UNMARKED;
        return 0;
    }
    return cached_rom_system(path, info, screens, rank);
}

int rom_catalog_probe(const char *path, uint32_t *screens) {
    struct stat info;
    uint32_t supported_screens;
    int rank;
    if (stat(path, &info) != 0) {
        if (screens) *screens = 0;
        return 0;
    }
    int system = probe_rom(path, &info, &supported_screens, &rank);
    if (screens) *screens = supported_screens;
    return system;
}

uint32_t rom_catalog_label(const char *path, char *label, size_t label_size) {
    struct stat info;
    uint32_t screens;
    int rank;
    if (stat(path, &info) != 0) return 0;
    int system = probe_rom(path, &info, &screens, &rank);
    if (!system) return 0;
    static const char *RANKS[] = { "", " (patched)", " (patched, 115200)" };
    snprintf(label, label_size, "%s: %s%s", system == 1 ? "CE 1.0" : "CE 2.0", file_leaf_name(path), RANKS[rank]);
    return screens;
}

void rom_catalog_find(rom_set_t *roms, const char *folder) {
    memset(roms, 0, sizeof *roms);
    DIR *dir = opendir(folder);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[sizeof roms->path[0]];
        if (snprintf(path, sizeof path, "%s/%s", folder, entry->d_name) >= (int)sizeof path) continue;
        struct stat info;
        if (stat(path, &info) != 0) continue;
        uint32_t screens;
        int rank;
        int system = probe_rom(path, &info, &screens, &rank);
        if (!system) continue;
        size_t size = (size_t)info.st_size;
        bool better = !roms->path[system][0] || rank > roms->rank[system] || (rank == roms->rank[system] && size > roms->size[system]);
        if (!better) continue;
        snprintf(roms->path[system], sizeof roms->path[system], "%s", path);
        roms->size[system] = size;
        roms->rank[system] = rank;
    }
    closedir(dir);
}
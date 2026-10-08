#include "app/machine_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/paths.h"
#include "util/file.h"

void machine_session_state_path(char *path, size_t size, machine_t *machine, const char *rom_path) {
    char base[1024];
    app_data_folder(base, sizeof base);
    char rom_name[256];
    snprintf(rom_name, sizeof rom_name, "%s", file_leaf_name(rom_path));
    char *extension = strrchr(rom_name, '.');
    if (extension && extension != rom_name) *extension = 0;
    snprintf(path, size, "%s/state-%s-%08x.bin", base, rom_name, (uint32_t)machine_rom_hash(machine));
    FILE *existing = fopen(path, "rb");
    if (existing) { fclose(existing); return; }
    char legacy[1100];
    snprintf(legacy, sizeof legacy, "%s/state.bin", base);
    if (machine_state_matches(machine, legacy)) rename(legacy, path);
}

static bool legacy_state_path(const char *rom_path, char *state, size_t size) {
    size_t rom_size;
    uint8_t *rom = file_read(rom_path, &rom_size);
    if (!rom) return false;
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) return false;
    machine_session_state_path(state, size, machine, rom_path);
    machine_destroy(machine);
    return true;
}

void machine_session_migrate_profiles(profiles_t *profiles, const rom_set_t *roms, const settings_t *settings, const char *folder) {
    static const char *NAMES[] = { "", "Windows CE 1.0", "Windows CE 2.0" };
    for (int system = 1; system <= 2; system++) {
        if (!roms->path[system][0]) continue;
        profile_t profile = { .memory = system == 2 ? CE2_DEFAULT_MEMORY : settings->memory, .screen = settings->screen, .host_time = settings->host_time != 0 };
        snprintf(profile.name, sizeof profile.name, "%s", NAMES[system]);
        snprintf(profile.rom, sizeof profile.rom, "%s", roms->path[system]);
        if (!legacy_state_path(profile.rom, profile.state, sizeof profile.state)) continue;
        profile_make_unique(profiles, &profile, folder);
        profile_save(&profile, folder);
        profiles_load(profiles, folder);
    }
}

machine_t *machine_session_start(const profile_t *profile, uint32_t speed, bool optimisations, const char *state_file, bool fresh,
                                 char *state, size_t state_size, const char **notice, snapshot_store_t *snapshots,
                                 const machine_session_hooks_t *hooks) {
    const char *rom_path = profile->rom;
    static char message[1400];
    *notice = NULL;
    size_t rom_size;
    uint8_t *rom = file_read(rom_path, &rom_size);
    if (!rom) {
        snprintf(message, sizeof message, "cannot read %s", rom_path);
        *notice = message;
        return NULL;
    }
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) {
        snprintf(message, sizeof message, "%s", error);
        *notice = message;
        return NULL;
    }
    machine_set_log(machine, hooks->log);
    machine_set_memory(machine, profile->memory);
    machine_set_screen(machine, profile->screen);
    machine_set_speed(machine, speed);
    machine_set_optimisations(machine, optimisations);
    machine_set_host_clock(machine, profile->host_time);
    machine_set_debug_output(machine, hooks->debug_output, NULL);
    hooks->start_debug_log(rom_path);
    if (state_file) snprintf(state, state_size, "%s", state_file);
    else if (profile->state[0]) snprintf(state, state_size, "%s", profile->state);
    else machine_session_state_path(state, state_size, machine, rom_path);
    if (fresh) {
        snapshot_store_backup_file(snapshots, state);
        return machine;
    }
    int64_t saved_at;
    if (machine_load(machine, state, &saved_at)) {
        machine_advance_clock(machine, (int64_t)time(NULL) - saved_at);
        return machine;
    }
    FILE *existing = fopen(state, "rb");
    if (existing && state_file) {
        fclose(existing);
        snprintf(message, sizeof message, "velo: cannot load %s with %s; it was saved with another ROM, or isn't a velo-emu state", state_file,
                 file_leaf_name(rom_path));
        *notice = message;
        machine_destroy(machine);
        return NULL;
    }
    if (!existing) hooks->insert_library_card(machine);
    if (existing) {
        fclose(existing);
        char backup[1200];
        snprintf(backup, sizeof backup, "%s.old", state);
        rename(state, backup);
        snprintf(message, sizeof message, "saved state unreadable, moved to %s", file_leaf_name(backup));
        *notice = message;
    }
    return machine;
}
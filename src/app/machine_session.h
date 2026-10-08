#pragma once

#include "app/profiles.h"
#include "app/rom_catalog.h"
#include "app/settings.h"
#include "app/snapshot_store.h"

#define CE2_DEFAULT_MEMORY 32

typedef struct {
    machine_log_fn log;
    machine_debug_fn debug_output;
    void (*start_debug_log)(const char *rom_path);
    void (*insert_library_card)(machine_t *machine);
} machine_session_hooks_t;

void       machine_session_state_path(char *path, size_t size, machine_t *machine, const char *rom_path);
void       machine_session_migrate_profiles(profiles_t *profiles, const rom_set_t *roms, const settings_t *settings, const char *folder);
machine_t *machine_session_start(const profile_t *profile, uint32_t speed, bool optimisations, const char *state_file, bool fresh,
                                 char *state, size_t state_size, const char **notice, snapshot_store_t *snapshots,
                                 const machine_session_hooks_t *hooks);
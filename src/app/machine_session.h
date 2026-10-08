#pragma once

#include "app/profiles.h"
#include "app/rom_catalog.h"
#include "app/settings.h"
#include "app/snapshot_store.h"

#define CE2_DEFAULT_MEMORY 32
#define MACHINE_SESSION_PATH_SIZE 1100

typedef struct {
    machine_t *machine;
    char state_path[MACHINE_SESSION_PATH_SIZE];
    bool new_state;
} machine_session_t;

typedef struct {
    machine_log_fn log;
    machine_debug_fn debug_output;
    void *debug_context;
} machine_session_output_t;

void       machine_session_state_path(char *path, size_t size, machine_t *machine, const char *rom_path);
void       machine_session_migrate_profiles(profiles_t *profiles, const rom_set_t *roms, const settings_t *settings, const char *folder);
bool       machine_session_start(machine_session_t *session, const profile_t *profile, uint32_t speed, bool optimisations,
                                 const char *state_file, bool fresh, snapshot_store_t *snapshots,
                                 const machine_session_output_t *output, char *notice, size_t notice_size);
void       machine_session_destroy(machine_session_t *session);
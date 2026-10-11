#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/desktop.h"
#include "app/input.h"
#include "app/launch.h"
#include "app/library.h"
#include "app/machine_session.h"
#include "app/notices.h"
#include "app/picks.h"
#include "app/profiles.h"
#include "app/runner.h"
#include "app/serial_service.h"
#include "app/settings.h"
#include "app/snapshot_store.h"
#include "app/typer.h"
#include "app/view.h"
#include "core/agent.h"
#include "core/gdb.h"
#include "core/machine.h"

#define SPEED_SETTLE_SECONDS      10ull
#define SOFT_RESET_REPLUG_SECONDS 2ull
#define POWER_PRESS_SECONDS       0.2
#define BACKLIGHT_PRESS_SECONDS   0.1
#define AUDIO_CHUNK               8192

typedef struct {
    settings_t settings;
    launch_t launch;
    snapshot_store_t snapshots;
    rom_set_t roms;
    char profiles_folder[1100];
    profiles_t profiles;
    profile_t current;
    int current_index;
    machine_session_output_t session_output;
    machine_session_t session;
    key_layout_t key_layout;
    SDL_Window *window;
    SDL_Renderer *renderer;
    view_t *view;
    SDL_AudioSpec audio_spec;
    SDL_AudioStream *audio;
    int16_t samples[AUDIO_CHUNK];
    bool sound, running, paused, pen_down;
    bool held[256];
    input_queue_t input;
    typer_t typer;
    scroller_t scroller;
    notice_t notice;
    serial_service_t serial;
    desktop_t *desktop;
    dropped_t dropped;
    picked_t *picked;
    gdb_t *debugger;
    agent_t *agent;
    app_runner_t *runner;
    uint64_t last_frame;
    double frequency, since_autosave, since_backup;
    uint64_t power_release_at, backlight_release_at;
} app_t;

gdb_t *app_start_network_gdb(machine_t *machine, uint32_t port, char *notice, size_t size);
bool   app_start_session(app_t *app, machine_session_t *session, const profile_t *profile, const char *state_file, bool fresh, char *notice, size_t notice_size);
void   app_command(app_t *app, int item, int *switch_to, bool *events_seen);
void   app_switch_machine(app_t *app, int index);
void   app_handle_picked(app_t *app, bool velo_online);
void   app_update_menus(app_t *app, bool velo_online);

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/android.h"
#include "app/capture.h"
#include "app/desktop.h"
#include "app/dialog.h"
#include "app/host.h"
#include "app/input.h"
#include "app/launch.h"
#include "app/library.h"
#include "app/log.h"
#include "app/machine_session.h"
#include "app/menu.h"
#include "app/notices.h"
#include "app/paths.h"
#include "app/picks.h"
#include "app/profiles.h"
#include "app/rom_catalog.h"
#include "app/runner.h"
#include "app/serial_service.h"
#include "app/settings.h"
#include "app/snapshot_store.h"
#include "app/typer.h"
#include "app/view.h"
#include "core/agent.h"
#include "core/gdb.h"
#include "core/key_text.h"
#include "core/lcd.h"
#include "core/machine.h"
#include "net/net_gateway.h"
#include "net/serial_link.h"
#include "rapi/rapi.h"
#include "util/fat.h"
#include "util/file.h"
#include "util/marker.h"
#include "util/options.h"
#include "util/png.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

#define WINDOW_SCALE     2
#define IDLE_FRAME_NS    (SDL_NS_PER_SECOND / 60)
#define MAX_FRAME_SLICE  0.1
#define AUTOSAVE_SECONDS 60
#define BACKUP_SECONDS   600
#define SPEED_SETTLE_SECONDS 10ull
#define SOFT_RESET_REPLUG_SECONDS 2ull
#define POWER_PRESS_SECONDS 0.2
#define BACKLIGHT_PRESS_SECONDS 0.1
#define AUDIO_CHUNK 8192
#define WINDOW_TITLE     "Philips Velo 1"
#define ANDROID_UNLIT_LEVEL 0.5f


typedef struct {
    settings_t settings;
    launch_t launch;
    snapshot_store_t snapshots;
    rom_set_t roms;
    char profiles_folder[1100];
    profiles_t profiles;
    profile_t current;
    int current_index;
    machine_session_hooks_t session_hooks;
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

static void release_keys(input_queue_t *input, machine_t *machine, bool *held, int only_modifiers_up) {
    static const struct { uint8_t scancode; int modifier; } modifiers[] = {
        { 0x51, MENU_MOD_SHIFT }, { 0x01, MENU_MOD_CONTROL }, { 0x19, MENU_MOD_ALT }, { 0x09, MENU_MOD_ALT },
    };
    if (only_modifiers_up < 0) {
        for (int i = 0; i < 256; i++) {
            if (!held[i]) continue;
            held[i] = false;
            input_add(input, machine, INPUT_KEY, false, 0, 0, (uint8_t)i);
        }
        return;
    }
    if (!(only_modifiers_up & MENU_MOD_KNOWN)) return;
    for (size_t i = 0; i < sizeof modifiers / sizeof modifiers[0]; i++) {
        uint8_t scancode = modifiers[i].scancode;
        if (held[scancode] && !(only_modifiers_up & modifiers[i].modifier)) {
            held[scancode] = false;
            input_add(input, machine, INPUT_KEY, false, 0, 0, scancode);
        }
    }
}

static bool confirm_reset(SDL_Window *window, const char *name) {
    char title[160];
    snprintf(title, sizeof title, "Reset %s?", name);
    return host_confirm(window, title,
                        "A reset is a cold boot back to the factory state: it clears RAM, including files, settings and installed programs. A backup of the machine goes in Snapshots/Backups first. Soft Reset keeps them.",
                        "Reset");
}

static gdb_t *start_network_gdb(machine_t *machine, uint32_t port, char *notice, size_t size) {
    gdb_t *gdb = gdb_create(machine, (int)port, true, app_log_always);
    char address[64];
    host_local_address(address, sizeof address);
    if (gdb) snprintf(notice, size, "GDB server at %s:%u", address, port);
    else snprintf(notice, size, "cannot listen for GDB on port %u", port);
    return gdb;
}

static void set_title(SDL_Window *window, const char *name, const char *notice, bool paused, bool suspended) {
    char base[200], title[1400];
    snprintf(base, sizeof base, "%s (%s)", WINDOW_TITLE, name);
    if (notice) snprintf(title, sizeof title, "%s: %s", base, notice);
    else if (paused) snprintf(title, sizeof title, "%s, paused", base);
    else if (suspended) snprintf(title, sizeof title, "%s, suspended", base);
    else snprintf(title, sizeof title, "%s", base);
    if (strcmp(SDL_GetWindowTitle(window), title)) SDL_SetWindowTitle(window, title);
}

const uint32_t DIALOG_MEMORY_SIZES[DIALOG_MEMORY_COUNT] = { 4, 8, 16, 20, 32 };

static int profile_system(const profile_t *profile) {
    return rom_catalog_probe(profile->rom, NULL);
}

static void window_size(view_display_t display, uint32_t scale, int *width, int *height) {
    view_source_size(display, width, height);
    *width = *width * WINDOW_SCALE * (int)scale / 100;
    *height = *height * WINDOW_SCALE * (int)scale / 100 + menu_bar_height();
}

static void fit_window(SDL_Window *window, view_t *view, uint32_t scale) {
#ifdef __ANDROID__
    (void)window;
    (void)view;
    (void)scale;
    return;
#endif
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(window, false);
    int width, height;
    window_size(view_display(view), scale, &width, &height);
    SDL_SetWindowSize(window, width, height);
}

static void print_debug_line(void *context, const char *line) {
    app_t *app = context;
    if (app->debugger) gdb_debug_line(app->debugger, line);
    debug_log_line(line);
}

static bool poll_host_events(app_t *app) {
    machine_t *machine = app->session.machine;
    bool events_seen = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        events_seen = true;
        if (menu_event(&event)) {
            if (menu_active()) {
                release_keys(&app->input, machine, app->held, -1);
                if (app->pen_down) input_add(&app->input, machine, INPUT_PEN, false, 0, 0, 0);
                app->pen_down = false;
            }
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            app->running = false;
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            machine_save(machine, app->session.state_path, (int64_t)time(NULL));
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = event.type == SDL_EVENT_KEY_DOWN;
            uint8_t scancode;
            if (!input_find_scancode(app->key_layout, event.key.key, &scancode)) break;
            if (down) {
                if (event.key.repeat || (event.key.mod & SDL_KMOD_GUI) || app->held[scancode]) break;
                app->held[scancode] = true;
                input_add(&app->input, machine, INPUT_KEY, true, 0, 0, scancode);
            } else if (app->held[scancode]) {
                app->held[scancode] = false;
                input_add(&app->input, machine, INPUT_KEY, false, 0, 0, scancode);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            scroller_add(&app->scroller, event.wheel.y, event.wheel.x);
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data && app->dropped.count < PICK_MAX) snprintf(app->dropped.paths[app->dropped.count++], sizeof app->dropped.paths[0], "%s", event.drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE:
            if (app->dropped.count) {
                bool online = serial_service_online(&app->serial);
                notice_show(&app->notice, picks_handle_drop(&app->dropped, machine, app->desktop, online && !desktop_busy(app->desktop)), NOTICE_MEDIUM);
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            library_find_roms(&app->roms);
#ifdef __ANDROID__
            SDL_SetWindowFullscreen(app->window, false);
            SDL_SetWindowFullscreen(app->window, true);
#endif
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            release_keys(&app->input, machine, app->held, -1);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                int x, y;
                if (view_screen_position(app->view, event.button.x, event.button.y, &x, &y)) {
                    app->pen_down = true;
                    input_add(&app->input, machine, INPUT_PEN, true, x, y, 0);
                }
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (app->pen_down) {
                int x, y;
                view_screen_position(app->view, event.motion.x, event.motion.y, &x, &y);
                pen_move(&app->input, machine, x, y);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT && app->pen_down) {
                int x, y;
                view_screen_position(app->view, event.button.x, event.button.y, &x, &y);
                app->pen_down = false;
                input_add(&app->input, machine, INPUT_PEN, false, x, y, 0);
            }
            break;
        default: {
            picked_t *picked = picks_take(&event);
            if (picked) {
                free(app->picked);
                app->picked = picked;
            }
            break;
        }
        }
    }
    return events_seen;
}

static bool handle_machine_menu(app_t *app, int item, int *switch_to, bool *events_seen) {
    if (item == MENU_NEW_MACHINE) {
        dialog_rom_t rom_list[32] = { 0 };
        int rom_count = library_list_roms(rom_list, 32);
        dialog_machine_t chosen = { .memory = profile_system(&app->current) == 2 ? CE2_DEFAULT_MEMORY : 4,
                                    .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .host_time = app->settings.host_time != 0 };
        if (rom_count) snprintf(chosen.rom, sizeof chosen.rom, "%s", app->current.rom);
        *events_seen = true;
        if (!dialog_new_machine(app->window, rom_list, rom_count, rom_catalog_label, &chosen)) return true;
        profile_t made = { .screen = chosen.screen, .memory = chosen.memory, .host_time = chosen.host_time };
        snprintf(made.rom, sizeof made.rom, "%s", chosen.rom);
        if (chosen.name[0]) snprintf(made.name, sizeof made.name, "%s", chosen.name);
        else profile_default_name(&made, profile_system(&made), made.name, sizeof made.name);
        profile_make_unique(&app->profiles, &made, app->profiles_folder);
        if (!profile_save(&made, app->profiles_folder)) {
            notice_show(&app->notice, "could not save the new machine", NOTICE_MEDIUM);
            return true;
        }
        profiles_load(&app->profiles, app->profiles_folder);
        if (app->current.id[0]) app->current_index = profile_find(&app->profiles, app->current.id);
        *switch_to = profile_find(&app->profiles, made.id);
        return true;
    }
    if (item != MENU_MANAGE_MACHINES) return false;

    const char *names[PROFILES_MAX];
    for (int i = 0; i < app->profiles.count; i++) names[i] = app->profiles.entries[i].name;
    int chosen = app->current_index >= 0 ? app->current_index : 0;
    *events_seen = true;
    dialog_manage_t action = app->profiles.count ? dialog_manage_machines(app->window, names, app->profiles.count, app->current_index, &chosen) : DIALOG_MANAGE_CLOSE;
    if (action == DIALOG_MANAGE_CLOSE || chosen < 0 || chosen >= app->profiles.count) return true;
    profile_t picked_profile = app->profiles.entries[chosen];
    char message[300];
    if (action == DIALOG_MANAGE_RESET) {
        if (!confirm_reset(app->window, picked_profile.name)) return true;
        if (chosen == app->current_index) {
            snapshot_store_backup_machine(&app->snapshots, app->session.machine, app->session.state_path);
            app->since_backup = 0;
            machine_reset(app->session.machine);
        } else {
            snapshot_store_backup_file(&app->snapshots, picked_profile.state);
            remove(picked_profile.state);
        }
        snprintf(message, sizeof message, "reset %s; the machine before it is in Snapshots/Backups", picked_profile.name);
    } else {
        if (chosen == app->current_index) {
            notice_show(&app->notice, "switch to another machine before deleting this one", NOTICE_LONG);
            return true;
        }
        char title[160];
        snprintf(title, sizeof title, "Delete %s?", picked_profile.name);
        if (!host_confirm(app->window, title, "This removes the machine and its saved state. A backup of the state goes in Snapshots/Backups first.", "Delete")) return true;
        snapshot_store_backup_file(&app->snapshots, picked_profile.state);
        profile_delete(&picked_profile, app->profiles_folder);
        char current_id[sizeof app->current.id];
        snprintf(current_id, sizeof current_id, "%s", app->current.id);
        profiles_load(&app->profiles, app->profiles_folder);
        app->current_index = current_id[0] ? profile_find(&app->profiles, current_id) : -1;
        snprintf(message, sizeof message, "deleted %s", picked_profile.name);
    }
    notice_show(&app->notice, message, NOTICE_LONG);
    return true;
}

static bool handle_view_menu(app_t *app, int item) {
    settings_t *settings = &app->settings;
    switch (item) {
    case MENU_SCALE_50:
    case MENU_SCALE_75:
    case MENU_SCALE_100:
    case MENU_SCALE_150:
    case MENU_SCALE_200:
    case MENU_ZOOM_IN:
    case MENU_ZOOM_OUT: {
        int index = settings_scale_index(settings->scale);
        if (item == MENU_ZOOM_IN) index = index + 1 < SETTINGS_SCALE_COUNT ? index + 1 : index;
        else if (item == MENU_ZOOM_OUT) index = index > 0 ? index - 1 : index;
        else index = item - MENU_SCALE_50;
        settings->scale = settings_scale_at(index);
        settings_save(settings);
        fit_window(app->window, app->view, settings->scale);
        return true;
    }
    case MENU_FULL_SCREEN:
        SDL_SetWindowFullscreen(app->window, !(SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN));
        return true;
    case MENU_DISPLAY_SIMULATED:
    case MENU_DISPLAY_SHARP:
        settings->display = item == MENU_DISPLAY_SHARP ? VIEW_SHARP : VIEW_SIMULATED;
        settings_save(settings);
        view_set_display(app->view, (view_display_t)settings->display);
        fit_window(app->window, app->view, settings->scale);
        return true;
    case MENU_COPY_SCREEN:
        notice_show(&app->notice, capture_copy_screen(app->view) ? "screen copied" : "could not copy the screen", NOTICE_SHORT);
        return true;
    case MENU_SAVE_SCREENSHOT: {
        char path[1100];
        char message[1200];
        if (capture_save_screenshot(app->view, path, sizeof path)) snprintf(message, sizeof message, "saved %s", file_leaf_name(path));
        else snprintf(message, sizeof message, "could not save the screenshot");
        notice_show(&app->notice, message, NOTICE_MEDIUM);
        return true;
    }
    case MENU_CONNECT_AT_LAUNCH:
        settings->connect_at_launch = !settings->connect_at_launch;
        settings_save(settings);
        return true;
    }
    return false;
}

static void handle_menu(app_t *app, int item, int *switch_to, bool *events_seen) {
    if (handle_machine_menu(app, item, switch_to, events_seen)) return;
    if (handle_view_menu(app, item)) return;
    machine_t *machine = app->session.machine;
    settings_t *settings = &app->settings;
    switch (item) {
    case MENU_POWER:
        machine_power_button(machine, true);
        app->power_release_at = machine_cycles(machine) + (uint64_t)(POWER_PRESS_SECONDS * MACHINE_CLOCK_HZ);
        break;
    case MENU_PAUSE: app->paused = !app->paused; break;
    case MENU_SOFT_RESET:
        machine_soft_reset(machine);
        serial_service_replug(&app->serial, machine, SOFT_RESET_REPLUG_SECONDS);
        break;
    case MENU_PASTE: {
        char *clipboard = SDL_GetClipboardText();
        size_t typed = clipboard ? typer_start(&app->typer, app->key_layout, clipboard) : 0;
        SDL_free(clipboard);
        char message[64];
        snprintf(message, sizeof message, typed ? "typing %zu characters" : "nothing to type", typed);
        notice_show(&app->notice, message, NOTICE_SHORT);
        break;
    }
    case MENU_SAVE_STATE:
        notice_show(&app->notice, machine_save(machine, app->session.state_path, (int64_t)time(NULL)) ? "state saved" : "could not save state", NOTICE_SHORT);
        break;
    case MENU_LOAD_STATE:
        snapshot_store_backup_machine(&app->snapshots, machine, app->session.state_path);
        if (machine_load(machine, app->session.state_path, NULL)) {
            serial_service_restored(&app->serial, machine);
            notice_show(&app->notice, "state loaded", NOTICE_SHORT);
        } else {
            notice_show(&app->notice, "no saved state", NOTICE_SHORT);
        }
        break;
    case MENU_BACKLIGHT:
        machine_backlight_button(machine, true);
        app->backlight_release_at = machine_cycles(machine) + (uint64_t)(BACKLIGHT_PRESS_SECONDS * MACHINE_CLOCK_HZ);
        break;
    case MENU_SOUND: app->sound = !app->sound; break;
    case MENU_SHOW_STATE: host_reveal_file(app->session.state_path); break;
    case MENU_SAVE_SNAPSHOT: {
        static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state" } };
        static char default_snapshot[1200];
        snapshot_store_default_name(&app->snapshots, default_snapshot, sizeof default_snapshot);
        SDL_ShowSaveFileDialog(picks_done, (void *)(intptr_t)PICK_SAVE_SNAPSHOT, app->window, filters, 1, default_snapshot);
        break;
    }
    case MENU_LOAD_SNAPSHOT: {
        static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state;bin" } };
        static char folder[1100];
        snapshot_store_folder(&app->snapshots, folder, sizeof folder);
        SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_LOAD_SNAPSHOT, app->window, filters, 1, folder, false);
        break;
    }
    case MENU_SHOW_DEBUG_OUTPUT: {
        char path[1100];
        debug_log_path(path, sizeof path);
        debug_log_flush();
        host_open_path(path);
        break;
    }
    case MENU_QUIT:
        app->running = false;
        break;
#ifdef __ANDROID__
    case MENU_IMPORT: {
        int cards, imported = library_import_files(&cards);
        library_find_roms(&app->roms);
        char message[160];
        snprintf(message, sizeof message, "imported %d ROMs and %d cards", imported, cards);
        notice_show(&app->notice, message, NOTICE_MEDIUM);
        break;
    }
#endif
    case MENU_SPEED_1:
    case MENU_SPEED_2:
    case MENU_SPEED_4:
    case MENU_SPEED_8:
        settings->speed = item == MENU_SPEED_1 ? 1 : item == MENU_SPEED_2 ? 2 : item == MENU_SPEED_4 ? 4 : 8;
        machine_set_speed(machine, settings->speed);
        settings_save(settings);
        break;
    case MENU_OPTIMISATIONS:
        settings->optimisations = !settings->optimisations;
        machine_set_optimisations(machine, settings->optimisations);
        settings_save(settings);
        break;
    case MENU_INSERT_CARD: {
        static const SDL_DialogFileFilter filters[] = { { "Card images", "img;bin;raw" }, { "All files", "*" } };
        SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_CARD, app->window, filters, 2, NULL, false);
        break;
    }
    case MENU_INSERT_DISK: {
        static const SDL_DialogFileFilter filters[] = { { "Disk images", "img;bin;raw" }, { "All files", "*" } };
        SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_DISK, app->window, filters, 2, NULL, false);
        break;
    }
    case MENU_NEW_DISK: {
#ifdef __ANDROID__
        free(app->picked);
        app->picked = picks_new_disk();
        break;
#endif
        static const SDL_DialogFileFilter filters[] = { { "Disk images", "img" } };
        SDL_ShowSaveFileDialog(picks_done, (void *)(intptr_t)PICK_NEW_DISK, app->window, filters, 1, "Velo Disk.img");
        break;
    }
    case MENU_EJECT_DISK:
        machine_eject_disk(machine);
        notice_show(&app->notice, "disk ejected", NOTICE_SHORT);
        break;
    default:
        if (item >= MENU_MACHINE_FIRST && item <= MENU_MACHINE_LAST && item - MENU_MACHINE_FIRST < app->profiles.count && item - MENU_MACHINE_FIRST != app->current_index) {
            *switch_to = item - MENU_MACHINE_FIRST;
            break;
        }
        if (item >= MENU_SERIAL_PORT_FIRST && item <= MENU_SERIAL_PORT_LAST && item - MENU_SERIAL_PORT_FIRST < app->serial.port_count) {
            snprintf(settings->serial_device, sizeof settings->serial_device, "%s", app->serial.ports[item - MENU_SERIAL_PORT_FIRST]);
            settings_save(settings);
            notice_show(&app->notice, serial_service_select(&app->serial, machine, SERIAL_DEVICE), NOTICE_LONG);
        }
        break;
    case MENU_SERIAL_NETWORK:
    case MENU_SERIAL_PTY:
    case MENU_SERIAL_TCP:
    case MENU_SERIAL_OFF:
        notice_show(&app->notice,
                    serial_service_select(&app->serial, machine, item == MENU_SERIAL_NETWORK ? SERIAL_NETWORK : item == MENU_SERIAL_PTY ? SERIAL_PTY : item == MENU_SERIAL_TCP ? SERIAL_TCP : SERIAL_OFF), NOTICE_LONG);
        break;
#ifdef __ANDROID__
    case MENU_FULL_BRIGHTNESS:
        settings->full_brightness = !settings->full_brightness;
        settings_save(settings);
        break;
    case MENU_FETCH_DOCUMENTS:
    case MENU_SHARED_FOLDER: {
        if (!android_all_files_access()) {
            android_request_all_files_access();
            notice_show(&app->notice, "allow All files access for Velo, then try again", NOTICE_LONG);
            break;
        }
        bool shared = item == MENU_SHARED_FOLDER;
        picked_t *folder = calloc(1, sizeof *folder);
        const char *start = shared && settings->shared_folder[0] ? settings->shared_folder : "/storage/emulated/0/Documents";
        if (folder && android_choose_folder(shared ? "Folder to share with My Documents" : "Folder to copy My Documents into", start, folder->paths[0], sizeof folder->paths[0])) {
            folder->kind = shared ? PICK_SHARED : PICK_FETCH;
            folder->count = 1;
            free(app->picked);
            app->picked = folder;
        } else {
            free(folder);
        }
        *events_seen = true;
        break;
    }
#endif
    case MENU_GDB_SERVER:
        if (app->debugger) {
            app_runner_set_debugger_locked(app->runner, NULL);
            gdb_destroy(app->debugger);
            app->debugger = NULL;
            settings->gdb_server = 0;
            notice_show(&app->notice, "GDB server stopped", NOTICE_LONG);
        } else {
            char message[160];
            app->debugger = start_network_gdb(machine, settings->gdb_port, message, sizeof message);
            app_runner_set_debugger_locked(app->runner, app->debugger);
            settings->gdb_server = app->debugger != NULL;
            notice_show(&app->notice, message, NOTICE_LONG);
        }
        settings_save(settings);
        break;
    case MENU_NETWORK_RAPI: {
        settings->network_rapi = !settings->network_rapi;
        settings_save(settings);
        char address[64];
        host_local_address(address, sizeof address);
        char message[160];
        if (settings->network_rapi) snprintf(message, sizeof message, "RAPI at %s:%u", address, settings->rapi_port);
        else snprintf(message, sizeof message, "RAPI over the network off");
        serial_service_update_rapi(&app->serial, machine);
        notice_show(&app->notice, message, NOTICE_LONG);
        break;
    }
    case MENU_SEND_FILES:
        SDL_ShowOpenFileDialog(picks_done, (void *)(intptr_t)PICK_SEND, app->window, NULL, 0, NULL, true);
        break;
#ifndef __ANDROID__
    case MENU_FETCH_DOCUMENTS:
        SDL_ShowOpenFolderDialog(picks_done, (void *)(intptr_t)PICK_FETCH, app->window, NULL, false);
        break;
    case MENU_SHARED_FOLDER:
        SDL_ShowOpenFolderDialog(picks_done, (void *)(intptr_t)PICK_SHARED, app->window, settings->shared_folder[0] ? settings->shared_folder : NULL, false);
        break;
#endif
    case MENU_SYNC_NOW:
        desktop_sync(app->desktop, settings->shared_folder);
        break;
    case MENU_SET_PROXY:
        desktop_set_proxy(app->desktop);
        break;
    case MENU_BAUD_19200:
    case MENU_BAUD_38400:
    case MENU_BAUD_57600:
    case MENU_BAUD_115200:
        desktop_set_baud(app->desktop, item == MENU_BAUD_19200 ? 19200 : item == MENU_BAUD_38400 ? 38400 : item == MENU_BAUD_57600 ? 57600 : 115200);
        break;
    case MENU_STOP_SHARING: {
        char message[1200];
        snprintf(message, sizeof message, "stopped sharing %s", file_leaf_name(settings->shared_folder));
        settings->shared_folder[0] = 0;
        settings_save(settings);
        notice_show(&app->notice, message, NOTICE_SHORT);
        break;
    }
    case MENU_EJECT_CARD:
        machine_eject_card(machine);
        notice_show(&app->notice, "card ejected", NOTICE_SHORT);
        break;
    }
}

static void switch_machine(app_t *app, int index) {
    if (desktop_busy(app->desktop)) {
        notice_show(&app->notice, "busy with a desktop transfer", NOTICE_SHORT);
        return;
    }
    const char *switch_notice = NULL;
    machine_session_t next_session = { 0 };
    profile_t next_profile = app->profiles.entries[index];
    bool next_started = machine_session_start(&next_session, &next_profile, app->settings.speed, app->settings.optimisations != 0,
                                              NULL, false, &switch_notice, &app->snapshots, &app->session_hooks);
    if (!next_started) {
        notice_show(&app->notice, switch_notice, NOTICE_MEDIUM);
        return;
    }
    machine_t *machine = app->session.machine;
    serial_mode_t mode = serial_service_detach(&app->serial, machine);
    if (app->pen_down) machine_touch(machine, false, 0, 0);
    input_clear(&app->input);
    app->pen_down = false;
    app->typer.length = app->typer.position = 0;
    machine_save(machine, app->session.state_path, (int64_t)time(NULL));
    machine_session_destroy(&app->session);
    app->session = next_session;
    machine = app->session.machine;
    app->current = next_profile;
    app->current_index = index;
    app->key_layout = machine_key_layout(machine);
    snprintf(app->settings.machine, sizeof app->settings.machine, "%s", app->current.id);
    settings_save(&app->settings);
    serial_service_attach(&app->serial, machine, mode);
    app->power_release_at = app->backlight_release_at = 0;
    app->since_backup = 0;
    app_runner_set_machine_locked(app->runner, machine);
    if (app->debugger) gdb_set_machine(app->debugger, machine);
    char message[160];
    snprintf(message, sizeof message, "switched to %s", app->current.name);
    notice_show(&app->notice, switch_notice ? switch_notice : message, NOTICE_MEDIUM);
}

static void handle_picked(app_t *app, bool velo_online) {
    machine_t *machine = app->session.machine;
    picked_t *picked = app->picked;
    if (picked->kind == PICK_CARD) {
        notice_show(&app->notice, machine_insert_card(machine, picked->paths[0]) ? "card inserted" : "could not open card image", NOTICE_SHORT);
    } else if (picked->kind == PICK_DISK) {
        notice_show(&app->notice, machine_insert_disk(machine, picked->paths[0], false) ? "disk inserted" : "could not open disk image", NOTICE_SHORT);
    } else if (picked->kind == PICK_NEW_DISK) {
        char path[1100];
        snprintf(path, sizeof path, "%s%s", picked->paths[0], file_has_extension(picked->paths[0], ".img") ? "" : ".img");
        bool made = picks_create_blank_disk(path) && machine_insert_disk(machine, path, false);
        char message[1200];
        snprintf(message, sizeof message, made ? "inserted new disk %s; the Velo offers to format it" : "could not create %s", file_leaf_name(path));
        notice_show(&app->notice, message, NOTICE_MEDIUM);
    } else if (picked->kind == PICK_SEND) {
        const char *files[PICK_MAX + 1];
        for (int i = 0; i < picked->count; i++) files[i] = picked->paths[i];
        files[picked->count] = NULL;
        desktop_send(app->desktop, files);
    } else if (picked->kind == PICK_SAVE_SNAPSHOT) {
        char path[1100];
        snprintf(path, sizeof path, "%s%s", picked->paths[0], file_has_extension(picked->paths[0], ".state") ? "" : ".state");
        bool saved = machine_save(machine, path, (int64_t)time(NULL));
#ifdef __ANDROID__
        if (saved && picked->export_uri[0]) {
            saved = android_export(path, picked->export_uri);
            remove(path);
        }
#endif
        char message[1200];
        snprintf(message, sizeof message, saved ? "saved snapshot %s" : "could not save %s", file_leaf_name(path));
        notice_show(&app->notice, message, NOTICE_MEDIUM);
    } else if (picked->kind == PICK_LOAD_SNAPSHOT) {
        char message[1200];
        if (machine_state_matches(machine, picked->paths[0])) snapshot_store_backup_machine(&app->snapshots, machine, app->session.state_path);
        if (machine_load(machine, picked->paths[0], NULL)) {
            serial_service_restored(&app->serial, machine);
            snprintf(message, sizeof message, "loaded snapshot %s", file_leaf_name(picked->paths[0]));
        } else {
            snprintf(message, sizeof message, "%s isn't a snapshot of this ROM", file_leaf_name(picked->paths[0]));
        }
        notice_show(&app->notice, message, NOTICE_MEDIUM);
    } else if (picked->kind == PICK_FETCH) {
        desktop_fetch(app->desktop, picked->paths[0]);
    } else if (picked->kind == PICK_SHARED) {
        snprintf(app->settings.shared_folder, sizeof app->settings.shared_folder, "%s", picked->paths[0]);
        settings_save(&app->settings);
        char message[1200];
        snprintf(message, sizeof message, "sharing %s with \\My Documents", file_leaf_name(app->settings.shared_folder));
        notice_show(&app->notice, message, NOTICE_MEDIUM);
        if (velo_online) desktop_sync(app->desktop, app->settings.shared_folder);
    }
    free(picked);
    app->picked = NULL;
}

static void poll_desktop(app_t *app) {
    machine_t *machine = app->session.machine;
    if (app->serial.link.gateway && net_gateway_take_desktop_connected(app->serial.link.gateway) && app->settings.shared_folder[0]) {
        desktop_sync(app->desktop, app->settings.shared_folder);
    }
    if (desktop_take_reconnect(app->desktop)) serial_service_replug(&app->serial, machine, SPEED_SETTLE_SECONDS);
    char message[256];
    if (desktop_take_status(app->desktop, message, sizeof message)) notice_show(&app->notice, message, NOTICE_MEDIUM);
}

static void update_menus(app_t *app, bool velo_online) {
    machine_t *machine = app->session.machine;
    const settings_t *settings = &app->settings;
    bool desktop_free = velo_online && !desktop_busy(app->desktop);
    menu_ensure();
    menu_set_enabled(MENU_SEND_FILES, desktop_free);
    menu_set_enabled(MENU_FETCH_DOCUMENTS, desktop_free);
    menu_set_enabled(MENU_SYNC_NOW, desktop_free && settings->shared_folder[0]);
    menu_set_enabled(MENU_STOP_SHARING, settings->shared_folder[0] != 0);
    menu_set_enabled(MENU_SET_PROXY, desktop_free);
    static const uint32_t LINK_SPEEDS[] = { 19200, 38400, 57600, 115200 };
    uint32_t link_baud = velo_online ? machine_serial_baud(machine) : 0;
    for (int baud_item = MENU_BAUD_19200; baud_item <= MENU_BAUD_115200; baud_item++) {
        uint32_t speed = LINK_SPEEDS[baud_item - MENU_BAUD_19200];
        menu_set_enabled(baud_item, desktop_free);
        menu_set_checked(baud_item, link_baud && link_baud * 20 > speed * 19 && link_baud * 20 < speed * 21);
    }
    menu_set_enabled(MENU_EJECT_CARD, machine_card_inserted(machine));
    menu_set_enabled(MENU_EJECT_DISK, machine_disk_inserted(machine));
    menu_set_checked(MENU_SERIAL_NETWORK, app->serial.link.mode == SERIAL_NETWORK);
    menu_set_checked(MENU_SERIAL_PTY, app->serial.link.mode == SERIAL_PTY);
    menu_set_checked(MENU_SERIAL_TCP, app->serial.link.mode == SERIAL_TCP);
    host_reap_children();
    for (int i = 0; i < SERIAL_PORT_MAX; i++) {
        int port_item = MENU_SERIAL_PORT_FIRST + i;
        bool shown = i < app->serial.port_count || (i == 0 && app->serial.port_count == 0);
        menu_set_hidden(port_item, !shown);
        if (!shown) continue;
        menu_set_title(port_item, app->serial.port_count ? app->serial.ports[i] + 5 : "No serial ports found");
        menu_set_enabled(port_item, app->serial.port_count > 0);
        menu_set_checked(port_item, app->serial.port_count && app->serial.link.mode == SERIAL_DEVICE && !strcmp(settings->serial_device, app->serial.ports[i]));
    }
    menu_set_checked(MENU_SERIAL_OFF, app->serial.link.mode == SERIAL_OFF);
    menu_set_checked(MENU_PAUSE, app->paused);
    menu_set_checked(MENU_BACKLIGHT, machine_backlight(machine));
    menu_set_checked(MENU_SOUND, app->sound);
    menu_set_checked(MENU_GDB_SERVER, app->debugger != NULL);
    menu_set_checked(MENU_NETWORK_RAPI, settings->network_rapi != 0);
    menu_set_checked(MENU_FULL_BRIGHTNESS, settings->full_brightness != 0);
    for (int i = 0; i < PROFILES_MAX; i++) {
        int machine_item = MENU_MACHINE_FIRST + i;
        menu_set_hidden(machine_item, i >= app->profiles.count);
        if (i >= app->profiles.count) continue;
        menu_set_title(machine_item, app->profiles.entries[i].name);
        menu_set_checked(machine_item, i == app->current_index);
    }
    menu_set_enabled(MENU_NEW_MACHINE, app->profiles.count < PROFILES_MAX);
    menu_set_checked(MENU_CONNECT_AT_LAUNCH, settings->connect_at_launch != 0);
    for (int scale_item = MENU_SCALE_50; scale_item <= MENU_SCALE_200; scale_item++) menu_set_checked(scale_item, settings->scale == settings_scale_at(scale_item - MENU_SCALE_50));
    menu_set_enabled(MENU_ZOOM_IN, settings->scale < settings_scale_at(SETTINGS_SCALE_COUNT - 1));
    menu_set_enabled(MENU_ZOOM_OUT, settings->scale > settings_scale_at(0));
    menu_set_checked(MENU_FULL_SCREEN, (SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN) != 0);
    menu_set_checked(MENU_DISPLAY_SIMULATED, settings->display == VIEW_SIMULATED);
    menu_set_checked(MENU_DISPLAY_SHARP, settings->display == VIEW_SHARP);
    menu_set_checked(MENU_SPEED_1, machine_speed(machine) == 1);
    menu_set_checked(MENU_SPEED_2, machine_speed(machine) == 2);
    menu_set_checked(MENU_SPEED_4, machine_speed(machine) == 4);
    menu_set_checked(MENU_SPEED_8, machine_speed(machine) == 8);
    menu_set_checked(MENU_OPTIMISATIONS, machine_optimisations(machine));
}

static void step_timers(app_t *app, double elapsed) {
    machine_t *machine = app->session.machine;
    app->since_autosave += elapsed;
    if (!app->paused) app->since_backup += elapsed;
    if (app->since_backup >= BACKUP_SECONDS) {
        app->since_backup = 0;
        snapshot_store_backup_machine(&app->snapshots, machine, app->session.state_path);
    }
    if (app->since_autosave >= AUTOSAVE_SECONDS) {
        app->since_autosave = 0;
        machine_save(machine, app->session.state_path, (int64_t)time(NULL));
    }
}

static void step_buttons(app_t *app) {
    machine_t *machine = app->session.machine;
    if (app->backlight_release_at && machine_cycles(machine) >= app->backlight_release_at) {
        app->backlight_release_at = 0;
        machine_backlight_button(machine, false);
    }
    if (app->power_release_at && machine_cycles(machine) >= app->power_release_at) {
        app->power_release_at = 0;
        machine_power_button(machine, false);
    }
}

static void step_audio(app_t *app) {
    uint32_t rate;
    for (size_t count; (count = machine_audio(app->session.machine, app->samples, AUDIO_CHUNK, &rate)) > 0;) {
        if (!app->audio || !app->sound) continue;
        if ((int)rate != app->audio_spec.freq) {
            app->audio_spec.freq = (int)rate;
            SDL_SetAudioStreamFormat(app->audio, &app->audio_spec, NULL);
        }
        SDL_PutAudioStreamData(app->audio, app->samples, (int)(count * sizeof app->samples[0]));
    }
}

static bool sync_screen(app_t *app) {
    machine_t *machine = app->session.machine;
    screen_size_t screen = machine_screen_size(machine);
    if (screen.width != lcd_width() || screen.height != lcd_height()) {
        view_set_screen_size(app->view, screen.width, screen.height);
        if (!(SDL_GetWindowFlags(app->window) & SDL_WINDOW_FULLSCREEN)) fit_window(app->window, app->view, app->settings.scale);
    }
    lcd_set_power(machine_lcd_enabled(machine));
    lcd_set_backlight(machine_backlight(machine));
#ifdef __ANDROID__
    android_update(app->settings.full_brightness && machine_backlight(machine) && machine_lcd_enabled(machine), !machine_suspended(machine));
#endif
    machine_screen(machine, lcd_framebuffer);
    return machine_lcd_enabled(machine);
}

static void run_frame(app_t *app) {
    uint64_t frame_start = SDL_GetTicksNS();
    app_runner_lock(app->runner);
    bool events_seen = poll_host_events(app);

    release_keys(&app->input, app->session.machine, app->held, menu_modifiers());
    int switch_to = -1;
    for (int item = menu_poll(); item >= 0; item = menu_poll()) {
        release_keys(&app->input, app->session.machine, app->held, -1);
        handle_menu(app, item, &switch_to, &events_seen);
    }
    if (switch_to >= 0 && switch_to < app->profiles.count && switch_to != app->current_index) switch_machine(app, switch_to);
    bool velo_online = serial_service_online(&app->serial);
    if (app->picked) handle_picked(app, velo_online);
    poll_desktop(app);
    update_menus(app, velo_online);

    uint64_t now = SDL_GetPerformanceCounter();
    double elapsed = (double)(now - app->last_frame) / app->frequency;
    app->last_frame = now;
    if (elapsed > MAX_FRAME_SLICE) elapsed = MAX_FRAME_SLICE;
    const char *notice = notice_current(&app->notice);
    set_title(app->window, app->current.name, notice, app->paused, machine_suspended(app->session.machine));
#ifdef __ANDROID__
    if (android_toast(notice ? notice : app->paused ? "Paused" : NULL)) events_seen = true;
#endif
    step_timers(app, elapsed);
    app_runner_set_paused_locked(app->runner, app->paused);
    typer_step(&app->typer, app->session.machine);
    scroller_step(&app->scroller, app->session.machine);
    serial_service_step(&app->serial, app->session.machine, elapsed, &app->notice);
    step_buttons(app);
    step_audio(app);
    bool lcd_on = sync_screen(app);
    app_runner_unlock(app->runner);

    int inset_left, inset_top, inset_right, inset_bottom;
    menu_insets(&inset_left, &inset_top, &inset_right, &inset_bottom);
    view_set_insets(app->view, inset_left, inset_top, inset_right, inset_bottom);
    bool screen_changed = view_update(app->view, (float)elapsed, lcd_on);
    if (screen_changed || events_seen || menu_active()) {
        view_render(app->view);
        menu_draw(app->renderer);
        SDL_RenderPresent(app->renderer);
    } else {
        uint64_t spent = SDL_GetTicksNS() - frame_start;
        if (spent < IDLE_FRAME_NS) SDL_DelayNS(IDLE_FRAME_NS - spent);
    }
}

static int load_launch(app_t *app, int argc, char **argv, bool *start) {
    *start = false;
#ifdef __ANDROID__
    const char *storage = SDL_GetAndroidExternalStoragePath();
    if (storage) {
        setenv("XDG_DATA_HOME", storage, 1);
        setenv("XDG_CONFIG_HOME", storage, 1);
    }
    const char *cache = SDL_GetAndroidCachePath();
    if (cache) setenv("TMPDIR", cache, 1);
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight Portrait");
#endif
    app_paths_migrate_old_folders();
    char base[1024];
    app_data_folder(base, sizeof base);
    snapshot_store_init(&app->snapshots, base);
    app->settings = settings_load();
    options_result_t parsed = launch_parse(&app->launch, &app->settings, argc, argv);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    app_log_set_verbose(app->launch.verbose);
    debug_log_set_stderr(app->launch.debug_output);
    *start = true;
    return 0;
}

static bool start_session(app_t *app, const char *state_file, bool fresh, const char **notice) {
    return machine_session_start(&app->session, &app->current, app->settings.speed, app->settings.optimisations != 0,
                                 state_file, fresh, notice, &app->snapshots, &app->session_hooks);
}

static int start_machine(app_t *app) {
    settings_t *settings = &app->settings;
    const launch_t *launch = &app->launch;
    app->session_hooks = (machine_session_hooks_t){ app_log, print_debug_line, debug_log_start, library_insert_card, app };
    library_find_roms(&app->roms);
    library_machines_folder(app->profiles_folder, sizeof app->profiles_folder);
    profiles_load(&app->profiles, app->profiles_folder);
    if (!app->profiles.count) machine_session_migrate_profiles(&app->profiles, &app->roms, settings, app->profiles_folder);
    app->current_index = -1;
    if (launch->rom) {
        app->current = (profile_t){ .memory = settings->memory, .screen = settings->screen, .host_time = settings->host_time != 0 };
        snprintf(app->current.rom, sizeof app->current.rom, "%s", launch->rom);
        snprintf(app->current.name, sizeof app->current.name, "%s", file_leaf_name(launch->rom));
    } else {
        if (launch->machine) {
            app->current_index = profile_find(&app->profiles, launch->machine);
            if (app->current_index < 0) {
                fprintf(stderr, "no machine called %s\n", launch->machine);
                return 2;
            }
        } else {
            app->current_index = settings->machine[0] ? profile_find(&app->profiles, settings->machine) : -1;
            if (app->current_index < 0) app->current_index = app->profiles.count ? 0 : -1;
        }
#ifdef __ANDROID__
        while (app->current_index < 0 && library_first_run_import()) {
            library_find_roms(&app->roms);
            machine_session_migrate_profiles(&app->profiles, &app->roms, settings, app->profiles_folder);
            app->current_index = app->profiles.count ? 0 : -1;
        }
#endif
        if (app->current_index < 0) {
            library_show_no_roms();
            return 1;
        }
        app->current = app->profiles.entries[app->current_index];
    }
    const char *notice;
    char fallback_notice[1600];
    bool started = start_session(app, launch->state_file, launch->fresh, &notice);
    if (!started && app->current_index >= 0 && !launch->machine) {
        snprintf(fallback_notice, sizeof fallback_notice, "Couldn't start %s: %s", app->current.name, notice);
        fprintf(stderr, "%s\n", fallback_notice);
        for (int i = 0; i < app->profiles.count && !started; i++) {
            if (i == app->current_index) continue;
            app->current = app->profiles.entries[i];
            started = start_session(app, NULL, false, &notice);
            if (started) {
                app->current_index = i;
                notice = fallback_notice;
            }
        }
    }
    if (!started) {
        fprintf(stderr, "%s\n", notice);
        return 1;
    }
    if (app->current_index >= 0) {
        snprintf(settings->machine, sizeof settings->machine, "%s", app->current.id);
        settings_save(settings);
    }
    notice_show(&app->notice, notice, NOTICE_LONG);
    return 0;
}

static bool open_window(app_t *app) {
    machine_t *machine = app->session.machine;
    app->key_layout = machine_key_layout(machine);
    screen_size_t screen = machine_screen_size(machine);
    lcd_set_size(screen.width, screen.height);

    SDL_SetAppMetadata("Velo", options_version(), "velo-emu");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    picks_init();
    int window_width, window_height;
    window_size((view_display_t)app->settings.display, app->settings.scale, &window_width, &window_height);
    app->window = SDL_CreateWindow("Philips Velo 1", window_width, window_height, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    app->renderer = app->window ? SDL_CreateRenderer(app->window, NULL) : NULL;
    if (!app->renderer) {
        fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderVSync(app->renderer, 1);
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(app->window, true);
    lcd_set_unlit_level(ANDROID_UNLIT_LEVEL);
#endif
    app->view = view_create(app->window, app->renderer, (view_display_t)app->settings.display, menu_bar_height());
    return true;
}

static void open_audio(app_t *app) {
    app->audio_spec = (SDL_AudioSpec){ SDL_AUDIO_S16, 1, 11025 };
    app->audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &app->audio_spec, NULL, NULL);
    if (app->audio) SDL_ResumeAudioStreamDevice(app->audio);
    else if (app_log_verbose()) fprintf(stderr, "audio: %s\n", SDL_GetError());
    app->sound = true;
}

static void open_serial(app_t *app) {
    char rapi_socket[1024], sync_manifest[1024];
    app_rapi_socket_path(rapi_socket, sizeof rapi_socket);
    rapi_data_path("sync-manifest.txt", sync_manifest, sizeof sync_manifest);
    app->desktop = desktop_create(rapi_socket, sync_manifest);
    serial_service_init(&app->serial, &app->settings);
    const char *result = serial_service_start(&app->serial, app->session.machine, app->launch.serial_mode);
    if (!notice_current(&app->notice)) notice_show(&app->notice, result, NOTICE_MEDIUM);
}

static int start_debugging(app_t *app) {
    machine_t *machine = app->session.machine;
    const launch_t *launch = &app->launch;
    if (launch->gdb_process && !launch->gdb_port) {
        fprintf(stderr, "velo: --gdb-process needs --gdb\n");
        return 2;
    }
    if (launch->gdb_port) {
        app->debugger = gdb_create(machine, launch->gdb_port, false, app_log_always);
        if (!app->debugger) {
            fprintf(stderr, "velo: cannot listen for GDB on port %d\n", launch->gdb_port);
            return 1;
        }
        if (launch->gdb_process) gdb_set_process(app->debugger, launch->gdb_process);
    }
    if (!app->debugger && app->settings.gdb_server) {
        char message[160];
        app->debugger = start_network_gdb(machine, app->settings.gdb_port, message, sizeof message);
        if (!notice_current(&app->notice)) notice_show(&app->notice, message, NOTICE_LONG);
    }
    if (launch->agent_socket && !(app->agent = agent_create(launch->agent_socket, app_log_always))) {
        fprintf(stderr, "velo: cannot listen on agent socket %s\n", launch->agent_socket);
        return 1;
    }
    return 0;
}

static void shut_down(app_t *app) {
    machine_t *machine = app->session.machine;
    app_runner_destroy(app->runner);
    gdb_destroy(app->debugger);
    app->debugger = NULL;
    agent_destroy(app->agent);
    app->agent = NULL;
    free(app->picked);
    machine_save(machine, app->session.state_path, (int64_t)time(NULL));
    serial_service_close(&app->serial, machine);
    desktop_destroy(app->desktop);
    if (app_log_verbose()) machine_dump_state(machine);
    SDL_DestroyAudioStream(app->audio);
    view_destroy(app->view);
    SDL_DestroyRenderer(app->renderer);
    SDL_DestroyWindow(app->window);
    SDL_Quit();
    machine_session_destroy(&app->session);
}

int main(int argc, char **argv) {
    app_t *app = calloc(1, sizeof *app);
    if (!app) return 1;
    bool start;
    int result = load_launch(app, argc, argv, &start);
    if (result || !start) return result;
    result = start_machine(app);
    if (result) return result;
    if (!open_window(app)) return 1;
    machine_t *machine = app->session.machine;
    if (app->launch.card && !machine_insert_card(machine, app->launch.card)) fprintf(stderr, "cannot open card image %s\n", app->launch.card);
    if (app->launch.disk && !machine_insert_disk(machine, app->launch.disk, false)) fprintf(stderr, "cannot open disk image %s\n", app->launch.disk);
    menu_install(app->window);
    open_audio(app);
    app->running = true;
    app->last_frame = SDL_GetPerformanceCounter();
    app->frequency = (double)SDL_GetPerformanceFrequency();
    open_serial(app);
    result = start_debugging(app);
    if (result) return result;
    app->runner = app_runner_create(machine, &app->input, app->debugger, app->agent);
    if (!app->runner) {
        fprintf(stderr, "velo: cannot start machine runner\n");
        return 1;
    }
    while (app->running) run_frame(app);
    shut_down(app);
    free(app);
    return 0;
}

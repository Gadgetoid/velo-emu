#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/android.h"
#include "app/desktop.h"
#include "app/dialog.h"
#include "app/input.h"
#include "app/machine_session.h"
#include "app/menu.h"
#include "app/notices.h"
#include "app/paths.h"
#include "app/profiles.h"
#include "app/rom_catalog.h"
#include "app/runner.h"
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
#ifdef __APPLE__
#define SCREENSHOT_FOLDER SDL_FOLDER_DESKTOP
#else
#define SCREENSHOT_FOLDER SDL_FOLDER_PICTURES
#endif

static bool verbose = false;

static gdb_t *debugger;
static agent_t *agent;
static snapshot_store_t snapshots;

static void log_gdb(const char *message) {
#ifdef __ANDROID__
    SDL_Log("%s", message);
#endif
    fputs(message, stderr);
}

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

#define SERIAL_PORT_MAX   16
#define PORT_SCAN_SECONDS 2.0

static void serial_log(const char *message) {
#ifdef __ANDROID__
    SDL_Log("%s", message);
#endif
    if (verbose) fputs(message, stderr);
}

static void rapi_socket_path(char *path, size_t size) {
#ifdef __ANDROID__
    net_gateway_socket_path(path, size, "velo-rapi");
#else
    rapi_data_path("rapi.sock", path, size);
#endif
}

static void local_address(char *address, size_t size);

static void serial_close(serial_link_t *serial, machine_t *machine) {
    serial_link_close(serial);
    machine_serial_connect(machine, false);
}

static bool serial_keeps_link(const serial_link_t *serial, serial_mode_t mode, const char *device) {
    if (mode == SERIAL_OFF || mode == SERIAL_NETWORK || serial->mode != mode) return false;
    return mode != SERIAL_DEVICE || (device && !strcmp(serial->name, device));
}

static const char *serial_open(serial_link_t *serial, machine_t *machine, serial_mode_t mode, const char *device) {
    static char rapi_socket[1024];
    static char notice[SERIAL_LINK_PORT_NAME + 16];
    if (serial_keeps_link(serial, mode, device)) {
        machine_serial_connect(machine, false);
    } else {
        serial_close(serial, machine);
        if (mode == SERIAL_NETWORK) {
            rapi_socket_path(rapi_socket, sizeof rapi_socket);
            serial->options.rapi_socket = rapi_socket;
        }
        const char *failure = serial_link_open(serial, mode, device);
        if (failure) return failure;
        if (mode == SERIAL_PTY) fprintf(stderr, "serial: COM1 on %s\n", serial->name);
    }
    machine_set_serial_tag(machine, (uint32_t)mode);
    if (mode != SERIAL_OFF && serial_link_attached(serial)) machine_serial_connect(machine, true);
    if (mode == SERIAL_NETWORK) return "network cable connected";
    if (mode == SERIAL_TCP) {
        char address[64];
        local_address(address, sizeof address);
        snprintf(notice, sizeof notice, "COM1 at %s:%d", address, serial->tcp_port);
        return notice;
    }
    if (mode == SERIAL_PTY) return serial->name;
    if (mode == SERIAL_DEVICE) {
        snprintf(notice, sizeof notice, "COM1 on %s", serial->name);
        return notice;
    }
    return "serial disconnected";
}

static void serial_restored(serial_link_t *serial, machine_t *machine, const char *device, uint64_t *reconnect_at, serial_mode_t *reconnect_mode) {
    bool was_connected = machine_serial_connected(machine);
    serial_mode_t mode = (serial_mode_t)machine_serial_tag(machine);
    machine_serial_connect(machine, false);
    *reconnect_at = 0;
    bool listening = mode == SERIAL_TCP && serial->mode == SERIAL_TCP;
    if (listening || (was_connected && (mode == SERIAL_NETWORK || mode == SERIAL_PTY || mode == SERIAL_TCP || (mode == SERIAL_DEVICE && device && device[0])))) {
        *reconnect_mode = mode;
        *reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
        if (!serial_keeps_link(serial, mode, device)) serial_link_close(serial);
    } else {
        serial_link_close(serial);
    }
}

typedef enum { PICK_SEND = 1, PICK_FETCH, PICK_SHARED, PICK_SAVE_SNAPSHOT, PICK_LOAD_SNAPSHOT, PICK_CARD, PICK_DISK, PICK_NEW_DISK } pick_kind_t;

#define PICK_MAX 64

typedef struct {
    pick_kind_t kind;
    int count;
    char paths[PICK_MAX][1024];
    char export_uri[1024];
} picked_t;

static Uint32 pick_event_type = 0;

static void pick_done(void *userdata, const char *const *files, int filter) {
    (void)filter;
    if (!pick_event_type || !files || !files[0]) return;
    picked_t *picked = malloc(sizeof *picked);
    if (!picked) return;
    picked->kind = (pick_kind_t)(intptr_t)userdata;
    picked->count = 0;
    picked->export_uri[0] = 0;
    while (files[picked->count] && picked->count < PICK_MAX) {
        snprintf(picked->paths[picked->count], sizeof picked->paths[0], "%s", files[picked->count]);
        picked->count++;
    }
    SDL_Event event;
    SDL_zero(event);
    event.type = pick_event_type;
    event.user.data1 = picked;
    if (!SDL_PushEvent(&event)) free(picked);
}

typedef struct {
    char paths[PICK_MAX][1024];
    int count;
} dropped_t;

#define BLANK_DISK_BYTES (32 * 1024 * 1024)

static bool create_blank_disk(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) return false;
    bool sized = fseek(file, BLANK_DISK_BYTES - 1, SEEK_SET) == 0 && fputc(0, file) == 0;
    return fclose(file) == 0 && sized;
}

static bool has_extension(const char *path, const char *extension) {
    const char *dot = strrchr(path, '.');
    return dot && !strcasecmp(dot, extension);
}

static bool is_directory(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static const char *handle_drop(dropped_t *dropped, machine_t *machine, desktop_t *desktop, bool online) {
    static char message[1200];
    int files = 0, scripts = -1, cards = -1;
    const char *list[PICK_MAX + 1];
    for (int i = 0; i < dropped->count; i++) {
        const char *path = dropped->paths[i];
        if (is_directory(path)) continue;
        if (has_extension(path, ".img") && cards < 0) cards = i;
        else if (has_extension(path, ".load") && scripts < 0) scripts = i;
        list[files++] = path;
    }
    list[files] = NULL;
    dropped->count = 0;
    if (cards >= 0 && files == 1) {
        snprintf(message, sizeof message, machine_insert_card(machine, list[0]) ? "inserted %s" : "could not open %s", file_leaf_name(list[0]));
        return message;
    }
    if (!files) return "drop files, a .load script or a card image";
    if (!online) return "connect Devices > Network (PPP) to send files to the Velo";
    if (scripts >= 0) {
        snprintf(message, sizeof message, "installing %s", file_leaf_name(dropped->paths[scripts]));
        return desktop_load(desktop, dropped->paths[scripts]) ? message : "busy with the last transfer";
    }
    return desktop_send(desktop, list) ? "sending to \\My Documents" : "busy with the last transfer";
}

static void log_message(const char *message) {
#ifdef __ANDROID__
    SDL_Log("%s", message);
#endif
    if (verbose) fputs(message, stderr);
}

#define DEBUG_LOG_MAX (1024 * 1024)

static FILE *debug_log;
static bool debug_to_stderr;

static void debug_log_path(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/debug.log", base);
}

static void print_debug_line(void *context, const char *line) {
    (void)context;
    if (debugger) gdb_debug_line(debugger, line);
    if (debug_to_stderr) fprintf(stderr, "debug: %s\n", line);
    if (!debug_log) return;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[16];
    strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
    fprintf(debug_log, "%s %s\n", stamp, line);
    fflush(debug_log);
}

static void start_debug_log(const char *rom_path) {
    if (!debug_log) {
        char path[1100], old[1110];
        debug_log_path(path, sizeof path);
        struct stat info;
        if (stat(path, &info) == 0 && info.st_size > DEBUG_LOG_MAX) {
            snprintf(old, sizeof old, "%s.old", path);
            rename(path, old);
        }
        debug_log = fopen(path, "a");
        if (!debug_log) return;
    }
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &local);
    fprintf(debug_log, "--- %s %s\n", stamp, file_leaf_name(rom_path));
    fflush(debug_log);
}

#ifdef __ANDROID__
static void localize_picked(picked_t *picked) {
    if (picked->kind == PICK_NEW_DISK) return;
    char folder[1100];
    if (picked->kind == PICK_CARD || picked->kind == PICK_DISK) {
        char base[1024];
        app_data_folder(base, sizeof base);
        snprintf(folder, sizeof folder, "%s/%s", base, picked->kind == PICK_CARD ? "cards" : "disks");
    } else {
        snprintf(folder, sizeof folder, "%s", getenv("TMPDIR") ? getenv("TMPDIR") : ".");
    }
    SDL_CreateDirectory(folder);
    for (int i = 0; i < picked->count; i++) {
        char uri[1024], path[1024];
        snprintf(uri, sizeof uri, "%s", picked->paths[i]);
        bool local;
        if (picked->kind == PICK_SAVE_SNAPSHOT) {
            local = android_local_path(uri, folder, path, sizeof path);
            snprintf(picked->export_uri, sizeof picked->export_uri, "%s", uri);
        } else {
            local = android_import(uri, folder, path, sizeof path);
        }
        snprintf(picked->paths[i], sizeof picked->paths[i], "%s", local ? path : "");
    }
}

static picked_t *new_disk_pick(void) {
    picked_t *picked = calloc(1, sizeof *picked);
    if (!picked) return NULL;
    char base[1024], folder[1100], stamp[64];
    app_data_folder(base, sizeof base);
    snprintf(folder, sizeof folder, "%s/disks", base);
    SDL_CreateDirectory(folder);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H.%M.%S", &local);
    picked->kind = PICK_NEW_DISK;
    picked->count = 1;
    snprintf(picked->paths[0], sizeof picked->paths[0], "%s/Velo Disk %s.img", folder, stamp);
    return picked;
}
#endif

static bool confirm_action(SDL_Window *window, const char *title, const char *message, const char *action) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, action },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_WARNING, window, title, message, (int)(sizeof buttons / sizeof buttons[0]), buttons, NULL };
    int chosen = 0;
    return SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1;
}

static bool confirm_reset(SDL_Window *window, const char *name) {
    char title[160];
    snprintf(title, sizeof title, "Reset %s?", name);
    return confirm_action(window, title,
                          "A reset is a cold boot back to the factory state: it clears RAM, including files, settings and installed programs. A backup of the machine goes in Snapshots/Backups first. Soft Reset keeps them.",
                          "Reset");
}

static void open_path(const char *path) {
    char url[4096] = "file://";
    size_t length = strlen(url);
    for (const unsigned char *at = (const unsigned char *)path; *at && length + 4 < sizeof url; at++) {
        if (isalnum(*at) || strchr("/-_.~", *at)) url[length++] = (char)*at;
        else length += (size_t)snprintf(url + length, sizeof url - length, "%%%02X", *at);
    }
    url[length] = 0;
    SDL_OpenURL(url);
}

#define REVEAL_CHILDREN_MAX 16

static pid_t reveal_children[REVEAL_CHILDREN_MAX];
static int reveal_child_count = 0;

static void reap_reveal_children(void) {
    int kept = 0;
    for (int i = 0; i < reveal_child_count; i++) {
        if (waitpid(reveal_children[i], NULL, WNOHANG) == 0) reveal_children[kept++] = reveal_children[i];
    }
    reveal_child_count = kept;
}

static void reveal_file(const char *path) {
#ifdef __APPLE__
    extern char **environ;
    char *arguments[] = { "open", "-R", (char *)path, NULL };
    pid_t pid;
    if (posix_spawnp(&pid, "open", NULL, NULL, arguments, environ) != 0) return;
    if (reveal_child_count < REVEAL_CHILDREN_MAX) reveal_children[reveal_child_count++] = pid;
    else waitpid(pid, NULL, 0);
#else
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder) *slash = 0;
    open_path(folder);
#endif
}

static void local_address(char *address, size_t size) {
    snprintf(address, size, "this computer");
    struct ifaddrs *interfaces;
    if (getifaddrs(&interfaces) != 0) return;
    int best = 0;
    for (struct ifaddrs *at = interfaces; at; at = at->ifa_next) {
        if (!at->ifa_addr || at->ifa_addr->sa_family != AF_INET || (at->ifa_flags & IFF_LOOPBACK) || !(at->ifa_flags & IFF_UP)) continue;
        int score = !strncmp(at->ifa_name, "wlan", 4) || !strcmp(at->ifa_name, "en0") ? 2 : 1;
        if (score <= best) continue;
        best = score;
        inet_ntop(AF_INET, &((struct sockaddr_in *)at->ifa_addr)->sin_addr, address, (socklen_t)size);
    }
    freeifaddrs(interfaces);
}

static gdb_t *start_network_gdb(machine_t *machine, uint32_t port, char *notice, size_t size) {
    gdb_t *gdb = gdb_create(machine, (int)port, true, log_gdb);
    char address[64];
    local_address(address, sizeof address);
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

static void rom_folder(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/roms", base);
    SDL_CreateDirectory(path);
}

#define CARD_MIN_BYTES  (1024 * 1024)

static void find_roms(rom_set_t *roms) {
    char folder[1100];
    rom_folder(folder, sizeof folder);
    rom_catalog_find(roms, folder);
}

static void cards_folder(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/cards", base);
    SDL_CreateDirectory(path);
}

static int library_score(const char *path, int system) {
    bool ce1 = fat_root_has_folder(path, "VELOLIB"), ce2 = fat_root_has_folder(path, "VELOLIB2");
    bool wanted = system == 2 ? ce2 : ce1, other = system == 2 ? ce1 : ce2;
    return wanted ? (other ? 1 : 2) : 0;
}

static void insert_library_card(machine_t *machine) {
    int system = machine_rom_system(machine);
    char folder[1100], best[1200] = "";
    cards_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return;
    int best_score = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[1200];
        if (snprintf(path, sizeof path, "%s/%s", folder, entry->d_name) >= (int)sizeof path) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < CARD_MIN_BYTES) continue;
        int score = library_score(path, system);
        if (score > best_score) {
            best_score = score;
            memcpy(best, path, sizeof best);
        }
    }
    closedir(dir);
    if (best[0] && machine_insert_card(machine, best)) fprintf(stderr, "inserted the software library %s\n", file_leaf_name(best));
}

static bool no_roms_dialog(void) {
    char folder[1100], message[1400];
    rom_folder(folder, sizeof folder);
    snprintf(message, sizeof message, "Put a Velo 1 ROM in %s: the CE 1.0 nk.bin, the merged CE 2.0 image, or both.", folder);
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Show ROM Folder" },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_INFORMATION, NULL, "No Velo ROM found", message, 2, buttons, NULL };
    int chosen = 0;
    if (SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1) open_path(folder);
    return false;
}

#ifdef __ANDROID__
typedef struct {
    SDL_AtomicInt done;
    int count;
    char uris[PICK_MAX][1024];
} import_pick_t;

static void import_picked(void *userdata, const char *const *files, int filter) {
    (void)filter;
    import_pick_t *pick = userdata;
    pick->count = 0;
    while (files && files[pick->count] && pick->count < PICK_MAX) {
        snprintf(pick->uris[pick->count], sizeof pick->uris[0], "%s", files[pick->count]);
        pick->count++;
    }
    SDL_SetAtomicInt(&pick->done, 1);
}

static int import_files(int *cards) {
    import_pick_t pick = { 0 };
    pick.count = 0;
    SDL_SetAtomicInt(&pick.done, 0);
    SDL_ShowOpenFileDialog(import_picked, &pick, NULL, NULL, 0, NULL, true);
    while (!SDL_GetAtomicInt(&pick.done)) {
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 100) && event.type == SDL_EVENT_QUIT) SDL_PushEvent(&event);
    }
    char roms[1100], card_folder[1100];
    rom_folder(roms, sizeof roms);
    cards_folder(card_folder, sizeof card_folder);
    int rom_count = 0;
    *cards = 0;
    for (int i = 0; i < pick.count; i++) {
        char path[1200];
        if (!android_import(pick.uris[i], roms, path, sizeof path)) continue;
        struct stat info;
        if (rom_catalog_probe(path, NULL)) {
            rom_count++;
            continue;
        }
        char card[1200];
        snprintf(card, sizeof card, "%s/%s", card_folder, file_leaf_name(path));
        if (stat(path, &info) == 0 && info.st_size >= CARD_MIN_BYTES && info.st_size % 512 == 0 && rename(path, card) == 0) (*cards)++;
        else remove(path);
    }
    return rom_count;
}

static bool first_run_import(void) {
    SDL_Init(SDL_INIT_VIDEO);
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose Files" },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_INFORMATION, NULL, "Import ROMs and Cards",
                                        "Choose your Velo 1 ROMs: the CE 1.0 nk.bin, a merged CE 2.0 image, or both. Card images, such as the Velo Software Library, can be chosen at the same time.",
                                        2, buttons, NULL };
    int chosen = 0;
    if (!SDL_ShowMessageBox(&dialog, &chosen) || chosen != 1) return false;
    int cards;
    import_files(&cards);
    return true;
}
#endif

const uint32_t DIALOG_MEMORY_SIZES[DIALOG_MEMORY_COUNT] = { 4, 8, 16, 20, 32 };

static void machines_folder(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/machines", base);
    SDL_CreateDirectory(path);
}

static uint32_t probe_rom(const char *path, char *label, size_t label_size) {
    return rom_catalog_label(path, label, label_size);
}

static int list_roms(dialog_rom_t *roms, int max) {
    char folder[1100];
    rom_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (entry->d_name[0] == '.') continue;
        dialog_rom_t *rom = &roms[count];
        if (snprintf(rom->path, sizeof rom->path, "%s/%s", folder, entry->d_name) >= (int)sizeof rom->path) continue;
        rom->screens = probe_rom(rom->path, rom->label, sizeof rom->label);
        if (rom->screens) count++;
    }
    closedir(dir);
    return count;
}

static int profile_system(const profile_t *profile) {
    return rom_catalog_probe(profile->rom, NULL);
}

typedef struct {
    machine_t   **machine;
    key_layout_t *key_layout;
    input_queue_t *input;
    scroller_t  *scroller;
    SDL_Window  *window;
    view_t      *view;
    bool        *running;
    bool        *pen_down;
    bool         *held;
    dropped_t   *dropped;
    picked_t   **picked;
    rom_set_t   *roms;
    machine_session_t *session;
    serial_link_t *serial;
    desktop_t   *desktop;
    notice_queue_t *notices;
} host_event_context_t;

static bool poll_host_events(host_event_context_t *context) {
    machine_t *machine = *context->machine;
    key_layout_t key_layout = *context->key_layout;
    bool events_seen = false;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        events_seen = true;
        if (menu_event(&event)) {
            if (menu_active()) {
                release_keys(context->input, machine, context->held, -1);
                if (*context->pen_down) input_add(context->input, machine, INPUT_PEN, false, 0, 0, 0);
                *context->pen_down = false;
            }
            continue;
        }
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_TERMINATING:
            *context->running = false;
            break;
        case SDL_EVENT_WILL_ENTER_BACKGROUND:
            machine_save(machine, context->session->state_path, (int64_t)time(NULL));
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            bool down = event.type == SDL_EVENT_KEY_DOWN;
            uint8_t scancode;
            if (!input_find_scancode(key_layout, event.key.key, &scancode)) break;
            if (down) {
                if (event.key.repeat || (event.key.mod & SDL_KMOD_GUI) || context->held[scancode]) break;
                context->held[scancode] = true;
                input_add(context->input, machine, INPUT_KEY, true, 0, 0, scancode);
            } else if (context->held[scancode]) {
                context->held[scancode] = false;
                input_add(context->input, machine, INPUT_KEY, false, 0, 0, scancode);
            }
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            scroller_add(context->scroller, event.wheel.y, event.wheel.x);
            break;
        case SDL_EVENT_DROP_FILE:
            if (event.drop.data && context->dropped->count < PICK_MAX) snprintf(context->dropped->paths[context->dropped->count++], sizeof context->dropped->paths[0], "%s", event.drop.data);
            break;
        case SDL_EVENT_DROP_COMPLETE:
            if (context->dropped->count) {
                bool online = context->serial->gateway && net_gateway_online(context->serial->gateway);
                notice_queue_push(context->notices,
                                  handle_drop(context->dropped, machine, context->desktop, online && !desktop_busy(context->desktop)));
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            find_roms(context->roms);
#ifdef __ANDROID__
            SDL_SetWindowFullscreen(context->window, false);
            SDL_SetWindowFullscreen(context->window, true);
#endif
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            release_keys(context->input, machine, context->held, -1);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button == SDL_BUTTON_LEFT) {
                int x, y;
                if (view_screen_position(context->view, event.button.x, event.button.y, &x, &y)) {
                    *context->pen_down = true;
                    input_add(context->input, machine, INPUT_PEN, true, x, y, 0);
                }
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (*context->pen_down) {
                int x, y;
                view_screen_position(context->view, event.motion.x, event.motion.y, &x, &y);
                pen_move(context->input, machine, x, y);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT && *context->pen_down) {
                int x, y;
                view_screen_position(context->view, event.button.x, event.button.y, &x, &y);
                *context->pen_down = false;
                input_add(context->input, machine, INPUT_PEN, false, x, y, 0);
            }
            break;
        default:
            if (pick_event_type && event.type == pick_event_type) {
                free(*context->picked);
                *context->picked = event.user.data1;
#ifdef __ANDROID__
                localize_picked(*context->picked);
#endif
            }
            break;
        }
    }
    return events_seen;
}

static const void *clipboard_png(void *userdata, const char *mime_type, size_t *size) {
    const size_t *stored = userdata;
    if (strcmp(mime_type, "image/png")) { *size = 0; return NULL; }
    *size = stored[0];
    return stored + 1;
}

static bool copy_screen(view_t *view) {
    int width, height;
    const uint32_t *pixels = view_image(view, &width, &height);
    uint8_t *png;
    size_t length;
    if (!pixels || !png_encode(pixels, width, height, &png, &length)) return false;
#ifdef __ANDROID__
    bool shared = android_share_picture(png, length, "Velo Screen.png");
    free(png);
    return shared;
#endif
    size_t *stored = malloc(sizeof(size_t) + length);
    if (!stored) { free(png); return false; }
    stored[0] = length;
    memcpy(stored + 1, png, length);
    free(png);
    const char *types[] = { "image/png" };
    if (SDL_SetClipboardData(clipboard_png, free, stored, types, 1)) return true;
    free(stored);
    return false;
}

static bool save_screenshot(view_t *view, char *path, size_t size) {
    int width, height;
    const uint32_t *pixels = view_image(view, &width, &height);
    uint8_t *png;
    size_t length;
    if (!pixels || !png_encode(pixels, width, height, &png, &length)) return false;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
#ifdef __ANDROID__
    snprintf(path, size, "Velo Screenshot %s.png", stamp);
    bool stored = android_save_picture(png, length, path);
    free(png);
    return stored;
#endif
    const char *folder = SDL_GetUserFolder(SCREENSHOT_FOLDER);
    if (folder) snprintf(path, size, "%sVelo Screenshot %s.png", folder, stamp);
    else snprintf(path, size, "%s/Velo Screenshot %s.png", getenv("HOME") ? getenv("HOME") : ".", stamp);
    FILE *file = fopen(path, "wb");
    bool saved = file && fwrite(png, 1, length, file) == length;
    if (file) fclose(file);
    free(png);
    return saved;
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

typedef struct {
    settings_t   *settings;
    serial_mode_t serial_mode;
    const char   *card, *disk, *state_file, *machine;
    bool fresh;
    int gdb_port;
    const char   *gdb_process;
    const char   *agent_socket;
} launch_t;

enum {
    LAUNCH_HEADING_MACHINE, LAUNCH_MACHINE, LAUNCH_STATE, LAUNCH_FRESH, LAUNCH_CARD, LAUNCH_DISK, LAUNCH_MEMORY, LAUNCH_SCREEN, LAUNCH_SPEED, LAUNCH_OPTIMISATIONS,
    LAUNCH_HEADING_CONNECTIONS, LAUNCH_SERIAL, LAUNCH_USER_AGENT, LAUNCH_AGENT,
    LAUNCH_HEADING_DEBUGGING, LAUNCH_VERBOSE, LAUNCH_DEBUG_OUTPUT, LAUNCH_GDB, LAUNCH_GDB_PROCESS,
};

static const option_t LAUNCH_OPTIONS[] = {
    [LAUNCH_HEADING_MACHINE] = { NULL, NULL, "Machine", 0 },
    [LAUNCH_MACHINE] = { "machine", "NAME", "open the machine with this name (from Machine > Machines)", 0 },
    [LAUNCH_STATE] = { "state", "FILE", "load, save and autosave FILE instead of the ROM's own state", 0 },
    [LAUNCH_FRESH] = { "fresh", NULL, "ignore the saved state and cold boot", 0 },
    [LAUNCH_CARD] = { "card", "IMAGE", "insert a PC Card image", 0 },
    [LAUNCH_DISK] = { "disk", "IMAGE", "attach a disk image to the paravirtual disk (needs vdisk.dll in the guest)", 0 },
    [LAUNCH_MEMORY] = { "memory", "MB", "RAM for a ROM given on the command line: 4, 8, 16, 20 or 32", 0 },
    [LAUNCH_SCREEN] = { "screen", "WxH", "screen for a ROM given on the command line: 480x240, 640x240, 640x480 or 800x600, where the ROM supports it", 0 },
    [LAUNCH_SPEED] = { "speed", "N", "CPU speed multiple: 1, 2, 4 or 8", 0 },
    [LAUNCH_OPTIMISATIONS] = { "optimisations", "on|off", "run CE's ROM compression natively and skip busy-waits on the clock", 0 },
    [LAUNCH_HEADING_CONNECTIONS] = { NULL, NULL, "Connections", 0 },
    [LAUNCH_SERIAL] = { "serial", "net|pty|tcp[:PORT]|off|PORT", "COM1 on the PPP network, a pseudo-terminal, a TCP port (9991 or PORT, kept in emu.ini), nothing, or a host serial port such as /dev/cu.usbserial-1", 0 },
    [LAUNCH_USER_AGENT] = { "user-agent", "TEXT", "the web proxy's user agent", 0 },
    [LAUNCH_AGENT] = { "agent", "SOCKET", "pass messages between a guest agent's break 0x51CE mailbox and one client on this Unix socket", 0 },
    [LAUNCH_HEADING_DEBUGGING] = { NULL, NULL, "Debugging", 0 },
    [LAUNCH_VERBOSE] = { "verbose", NULL, "log hardware, network and proxy activity to stderr", 0 },
    [LAUNCH_DEBUG_OUTPUT] = { "debug-output", NULL, "print CE's debug output (OutputDebugString, kernel messages) to stderr as well as debug.log", 0 },
    [LAUNCH_GDB] = { "gdb", "PORT", "listen for GDB on 127.0.0.1:PORT; it can attach and detach while the Velo runs", 0 },
    [LAUNCH_GDB_PROCESS] = { "gdb-process", "NAME", "debug one process, e.g. maths.exe: breakpoints below 0x02000000 only stop there, and GDB stops when it starts", 0 },
};

static bool launch_option(void *context, int option, const char *value, char *error, size_t error_size) {
    launch_t *launch = context;
    settings_t *settings = launch->settings;
    long integer;
    (void)error;
    (void)error_size;
    switch (option) {
    case LAUNCH_MACHINE: launch->machine = value; return true;
    case LAUNCH_STATE: launch->state_file = value; return true;
    case LAUNCH_FRESH: launch->fresh = true; return true;
    case LAUNCH_CARD: launch->card = value; return true;
    case LAUNCH_DISK: launch->disk = value; return true;
    case LAUNCH_MEMORY:
        if (!option_integer(value, 10, &integer) || (integer != 4 && integer != 8 && integer != 16 && integer != 20 && integer != 32)) return false;
        settings->memory = (uint32_t)integer;
        return true;
    case LAUNCH_SCREEN: return screen_parse(value, &settings->screen);
    case LAUNCH_SPEED:
        if (!option_integer(value, 10, &integer) || (integer != 1 && integer != 2 && integer != 4 && integer != 8)) return false;
        settings->speed = (uint32_t)integer;
        return true;
    case LAUNCH_OPTIMISATIONS:
        if (strcmp(value, "on") && strcmp(value, "off")) return false;
        settings->optimisations = !strcmp(value, "on");
        return true;
    case LAUNCH_SERIAL:
        if (!strcmp(value, "net")) launch->serial_mode = SERIAL_NETWORK;
        else if (!strcmp(value, "pty")) launch->serial_mode = SERIAL_PTY;
        else if (!strcmp(value, "tcp")) launch->serial_mode = SERIAL_TCP;
        else if (!strncmp(value, "tcp:", 4)) {
            long port;
            if (!option_integer(value + 4, 10, &port) || port < 1 || port > 65535) return false;
            settings->serial_tcp_port = (uint32_t)port;
            launch->serial_mode = SERIAL_TCP;
        }
        else if (!strcmp(value, "off")) launch->serial_mode = SERIAL_OFF;
        else if (value[0] == '/') {
            snprintf(settings->serial_device, sizeof settings->serial_device, "%s", value);
            launch->serial_mode = SERIAL_DEVICE;
        } else return false;
        return true;
    case LAUNCH_USER_AGENT: snprintf(settings->user_agent, sizeof settings->user_agent, "%s", value); return true;
    case LAUNCH_VERBOSE: verbose = true; return true;
    case LAUNCH_DEBUG_OUTPUT: debug_to_stderr = true; return true;
    case LAUNCH_GDB:
        if (!option_integer(value, 10, &integer) || integer < 1 || integer > 65535) return false;
        launch->gdb_port = (int)integer;
        return true;
    case LAUNCH_GDB_PROCESS: launch->gdb_process = value; return true;
    case LAUNCH_AGENT: launch->agent_socket = value; return true;
    }
    return false;
}

static const option_spec_t LAUNCH_SPEC = {
    "velo", "[OPTIONS] [ROM]",
    "Emulates a Philips Velo 1. With no ROM it opens the last machine used; machines are made with Machine > New Machine from the ROMs in the roms folder in its data folder. With a ROM it runs that ROM with its own saved state, outside the machine list.",
    LAUNCH_OPTIONS, (int)(sizeof LAUNCH_OPTIONS / sizeof LAUNCH_OPTIONS[0]),
    "headless runs the machine without a window, for tests and scripts, and velo-rapi talks to a running Velo.",
};

typedef struct {
    SDL_Window      *window;
    machine_t      **machine;
    settings_t      *settings;
    profiles_t      *profiles;
    profile_t       *current;
    int             *current_index;
    const char      *profiles_folder;
    snapshot_store_t *snapshots;
    notice_queue_t  *notices;
    machine_session_t *session;
    double          *since_backup;
} machine_menu_context_t;

static bool handle_machine_menu(machine_menu_context_t *context, int item, int *switch_to, bool *events_seen) {
    if (item == MENU_NEW_MACHINE) {
        dialog_rom_t rom_list[32] = { 0 };
        int rom_count = list_roms(rom_list, 32);
        dialog_machine_t chosen = { .memory = profile_system(context->current) == 2 ? CE2_DEFAULT_MEMORY : 4,
                                    .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .host_time = context->settings->host_time != 0 };
        if (rom_count) snprintf(chosen.rom, sizeof chosen.rom, "%s", context->current->rom);
        *events_seen = true;
        if (!dialog_new_machine(context->window, rom_list, rom_count, probe_rom, &chosen)) return true;
        profile_t made = { .screen = chosen.screen, .memory = chosen.memory, .host_time = chosen.host_time };
        snprintf(made.rom, sizeof made.rom, "%s", chosen.rom);
        if (chosen.name[0]) snprintf(made.name, sizeof made.name, "%s", chosen.name);
        else profile_default_name(&made, profile_system(&made), made.name, sizeof made.name);
        profile_make_unique(context->profiles, &made, context->profiles_folder);
        if (!profile_save(&made, context->profiles_folder)) {
            notice_queue_push(context->notices, "could not save the new machine");
            return true;
        }
        profiles_load(context->profiles, context->profiles_folder);
        if (context->current->id[0]) *context->current_index = profile_find(context->profiles, context->current->id);
        *switch_to = profile_find(context->profiles, made.id);
        return true;
    }
    if (item != MENU_MANAGE_MACHINES) return false;

    const char *names[PROFILES_MAX];
    for (int i = 0; i < context->profiles->count; i++) names[i] = context->profiles->entries[i].name;
    int chosen = *context->current_index >= 0 ? *context->current_index : 0;
    *events_seen = true;
    dialog_manage_t action = context->profiles->count ? dialog_manage_machines(context->window, names, context->profiles->count, *context->current_index, &chosen) : DIALOG_MANAGE_CLOSE;
    if (action == DIALOG_MANAGE_CLOSE || chosen < 0 || chosen >= context->profiles->count) return true;
    profile_t picked_profile = context->profiles->entries[chosen];
    char message[300];
    if (action == DIALOG_MANAGE_RESET) {
        if (!confirm_reset(context->window, picked_profile.name)) return true;
        if (chosen == *context->current_index) {
            snapshot_store_backup_machine(context->snapshots, *context->machine, context->session->state_path);
            *context->since_backup = 0;
            machine_reset(*context->machine);
        } else {
            snapshot_store_backup_file(context->snapshots, picked_profile.state);
            remove(picked_profile.state);
        }
        snprintf(message, sizeof message, "reset %s; the machine before it is in Snapshots/Backups", picked_profile.name);
    } else {
        if (chosen == *context->current_index) {
            notice_queue_push(context->notices, "switch to another machine before deleting this one");
            return true;
        }
        char title[160];
        snprintf(title, sizeof title, "Delete %s?", picked_profile.name);
        if (!confirm_action(context->window, title, "This removes the machine and its saved state. A backup of the state goes in Snapshots/Backups first.", "Delete")) return true;
        snapshot_store_backup_file(context->snapshots, picked_profile.state);
        profile_delete(&picked_profile, context->profiles_folder);
        char current_id[sizeof context->current->id];
        snprintf(current_id, sizeof current_id, "%s", context->current->id);
        profiles_load(context->profiles, context->profiles_folder);
        *context->current_index = current_id[0] ? profile_find(context->profiles, current_id) : -1;
        snprintf(message, sizeof message, "deleted %s", picked_profile.name);
    }
    notice_queue_push(context->notices, message);
    return true;
}

static bool handle_view_menu(settings_t *settings, SDL_Window *window, view_t *view, int item, notice_queue_t *notices) {
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
        fit_window(window, view, settings->scale);
        return true;
    }
    case MENU_FULL_SCREEN:
        SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
        return true;
    case MENU_DISPLAY_SIMULATED:
    case MENU_DISPLAY_SHARP:
        settings->display = item == MENU_DISPLAY_SHARP ? VIEW_SHARP : VIEW_SIMULATED;
        settings_save(settings);
        view_set_display(view, (view_display_t)settings->display);
        fit_window(window, view, settings->scale);
        return true;
    case MENU_COPY_SCREEN:
        notice_queue_push(notices, copy_screen(view) ? "screen copied" : "could not copy the screen");
        return true;
    case MENU_SAVE_SCREENSHOT: {
        char path[1100];
        char message[1200];
        if (save_screenshot(view, path, sizeof path)) snprintf(message, sizeof message, "saved %s", file_leaf_name(path));
        else snprintf(message, sizeof message, "could not save the screenshot");
        notice_queue_push(notices, message);
        return true;
    }
    case MENU_CONNECT_AT_LAUNCH:
        settings->connect_at_launch = !settings->connect_at_launch;
        settings_save(settings);
        return true;
    }
    return false;
}

static int initialize_launch(int argc, char **argv, settings_t *settings, launch_t *launch, const char **rom_path, bool *continue_start) {
    *rom_path = NULL;
    *continue_start = false;
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
    snapshot_store_init(&snapshots, base);
    *settings = settings_load();
    *launch = (launch_t){ settings, settings->connect_at_launch ? SERIAL_NETWORK : SERIAL_OFF,
                          NULL, NULL, NULL, NULL, false, 0, NULL, NULL };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&LAUNCH_SPEC, argc, argv, launch_option, launch, positional, 1, &positional_count);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    if (positional_count) *rom_path = positional[0];
    *continue_start = true;
    return 0;
}

typedef struct {
    rom_set_t roms;
    char profiles_folder[1100];
    profiles_t profiles;
    int current_index;
    machine_session_t session;
    const char *startup_notice;
    char fallback_notice[1600];
} machine_startup_t;

static int initialize_machine(machine_startup_t *startup, settings_t *settings, const launch_t *launch, const char *rom_path,
                              profile_t *current, const machine_session_hooks_t *session_hooks) {
    find_roms(&startup->roms);
    machines_folder(startup->profiles_folder, sizeof startup->profiles_folder);
    profiles_load(&startup->profiles, startup->profiles_folder);
    if (!startup->profiles.count) machine_session_migrate_profiles(&startup->profiles, &startup->roms, settings, startup->profiles_folder);
    startup->current_index = -1;
    if (rom_path) {
        *current = (profile_t){ .memory = settings->memory, .screen = settings->screen, .host_time = settings->host_time != 0 };
        snprintf(current->rom, sizeof current->rom, "%s", rom_path);
        snprintf(current->name, sizeof current->name, "%s", file_leaf_name(rom_path));
    } else {
        if (launch->machine) {
            startup->current_index = profile_find(&startup->profiles, launch->machine);
            if (startup->current_index < 0) {
                fprintf(stderr, "no machine called %s\n", launch->machine);
                return 2;
            }
        } else {
            startup->current_index = settings->machine[0] ? profile_find(&startup->profiles, settings->machine) : -1;
            if (startup->current_index < 0) startup->current_index = startup->profiles.count ? 0 : -1;
        }
#ifdef __ANDROID__
        while (startup->current_index < 0 && first_run_import()) {
            find_roms(&startup->roms);
            machine_session_migrate_profiles(&startup->profiles, &startup->roms, settings, startup->profiles_folder);
            startup->current_index = startup->profiles.count ? 0 : -1;
        }
#endif
        if (startup->current_index < 0) return no_roms_dialog() ? 0 : 1;
        *current = startup->profiles.entries[startup->current_index];
    }
    bool started = machine_session_start(&startup->session, current, settings->speed, settings->optimisations != 0,
                                         launch->state_file, launch->fresh, &startup->startup_notice, &snapshots, session_hooks);
    if (!started && startup->current_index >= 0 && !launch->machine) {
        snprintf(startup->fallback_notice, sizeof startup->fallback_notice, "Couldn't start %s: %s",
                 current->name, startup->startup_notice);
        fprintf(stderr, "%s\n", startup->fallback_notice);
        for (int i = 0; i < startup->profiles.count && !started; i++) {
            if (i == startup->current_index) continue;
            *current = startup->profiles.entries[i];
            started = machine_session_start(&startup->session, current, settings->speed, settings->optimisations != 0,
                                            NULL, false, &startup->startup_notice, &snapshots, session_hooks);
            if (started) {
                startup->current_index = i;
                startup->startup_notice = startup->fallback_notice;
            }
        }
    }
    if (!started) {
        fprintf(stderr, "%s\n", startup->startup_notice);
        return 1;
    }
    if (startup->current_index >= 0) {
        snprintf(settings->machine, sizeof settings->machine, "%s", current->id);
        settings_save(settings);
    }
    return 0;
}

int main(int argc, char **argv) {
    settings_t settings;
    launch_t launch;
    const char *rom_path;
    bool continue_start;
    int result = initialize_launch(argc, argv, &settings, &launch, &rom_path, &continue_start);
    if (result || !continue_start) return result;
    machine_session_hooks_t session_hooks = { log_message, print_debug_line, start_debug_log, insert_library_card };
    machine_startup_t machine_startup = { 0 };
    profile_t current = { 0 };
    result = initialize_machine(&machine_startup, &settings, &launch, rom_path, &current, &session_hooks);
    if (result) return result;
    serial_mode_t serial_mode = launch.serial_mode;
    const char *card = launch.card, *disk = launch.disk;
    rom_set_t roms = machine_startup.roms;
    char profiles_folder[sizeof machine_startup.profiles_folder];
    snprintf(profiles_folder, sizeof profiles_folder, "%s", machine_startup.profiles_folder);
    profiles_t profiles = machine_startup.profiles;
    int current_index = machine_startup.current_index;
    machine_session_t session = machine_startup.session;
    machine_startup.session.machine = NULL;
    const char *startup_notice = machine_startup.startup_notice;
    machine_t *machine = session.machine;
    key_layout_t key_layout = machine_key_layout(machine);
    screen_size_t screen = machine_screen_size(machine);
    lcd_set_size(screen.width, screen.height);

    SDL_SetAppMetadata("Velo", options_version(), "velo-emu");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    pick_event_type = SDL_RegisterEvents(1);
    int window_width, window_height;
    window_size((view_display_t)settings.display, settings.scale, &window_width, &window_height);
    SDL_Window *window = SDL_CreateWindow("Philips Velo 1", window_width, window_height, SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, NULL) : NULL;
    if (!renderer) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(renderer, 1);
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(window, true);
    lcd_set_unlit_level(ANDROID_UNLIT_LEVEL);
#endif

    view_t *view = view_create(window, renderer, (view_display_t)settings.display, menu_bar_height());


    if (card && !machine_insert_card(machine, card)) fprintf(stderr, "cannot open card image %s\n", card);
    if (disk && !machine_insert_disk(machine, disk, false)) fprintf(stderr, "cannot open disk image %s\n", disk);

    menu_install(window);

    SDL_AudioSpec audio_spec = { SDL_AUDIO_S16, 1, 11025 };
    SDL_AudioStream *audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, NULL, NULL);
    if (audio) SDL_ResumeAudioStreamDevice(audio);
    else if (verbose) fprintf(stderr, "audio: %s\n", SDL_GetError());
    bool sound = true;
    int16_t samples[AUDIO_CHUNK] = { 0 };

    bool running = true, pen_down = false, paused = false;
    bool held[256] = { false };
    uint64_t last = SDL_GetPerformanceCounter();
    double frequency = (double)SDL_GetPerformanceFrequency();
    double since_autosave = 0, since_backup = 0;
    uint64_t power_release_at = 0, backlight_release_at = 0;
    notice_queue_t notices;
    notice_queue_init(&notices);
    notice_queue_push(&notices, startup_notice);
    serial_link_t serial = { 0 };
    serial_link_init(&serial, serial_log);
    serial.options.user_agent = settings.user_agent;
    serial.options.rapi_port = settings.network_rapi ? (int)settings.rapi_port : 0;
    serial.tcp_port = (int)settings.serial_tcp_port;
    char rapi_socket[1024], sync_manifest[1024], desktop_notice[256], shared_notice[1200], paste_notice[64];
    typer_t typer = { 0 };
    scroller_t scroller = { 0 };
    input_queue_t input = { 0 };
    char ports[SERIAL_PORT_MAX][SERIAL_LINK_PORT_NAME] = { 0 };
    int port_count = 0;
    double since_port_scan = 0;
    dropped_t dropped = { 0 };
    picked_t *picked = NULL;
    rapi_socket_path(rapi_socket, sizeof rapi_socket);
    rapi_data_path("sync-manifest.txt", sync_manifest, sizeof sync_manifest);
    desktop_t *desktop = desktop_create(rapi_socket, sync_manifest);
    uint64_t serial_reconnect_at = 0, serial_unplug_at = 0;
    bool serial_tcp_attached = false;
    serial_mode_t serial_reconnect_mode = SERIAL_OFF;
    serial_restored(&serial, machine, settings.serial_device, &serial_reconnect_at, &serial_reconnect_mode);
    if (serial_mode != SERIAL_OFF && serial_reconnect_at) {
        serial_reconnect_mode = serial_mode;
    } else if (serial_mode != SERIAL_OFF) {
        const char *result = serial_open(&serial, machine, serial_mode, settings.serial_device);
        if (!notice_queue_current(&notices)) notice_queue_push(&notices, result);
    }

    if (launch.gdb_process && !launch.gdb_port) {
        fprintf(stderr, "velo: --gdb-process needs --gdb\n");
        return 2;
    }
    if (launch.gdb_port) {
        debugger = gdb_create(machine, launch.gdb_port, false, log_gdb);
        if (!debugger) {
            fprintf(stderr, "velo: cannot listen for GDB on port %d\n", launch.gdb_port);
            return 1;
        }
        if (launch.gdb_process) gdb_set_process(debugger, launch.gdb_process);
    }
    char gdb_notice[160] = { 0 };
    if (!debugger && settings.gdb_server) {
        debugger = start_network_gdb(machine, settings.gdb_port, gdb_notice, sizeof gdb_notice);
        if (!notice_queue_current(&notices)) notice_queue_push(&notices, gdb_notice);
    }
    if (launch.agent_socket && !(agent = agent_create(launch.agent_socket, log_gdb))) {
        fprintf(stderr, "velo: cannot listen on agent socket %s\n", launch.agent_socket);
        return 1;
    }
    app_runner_t *runner = NULL;
    runner = app_runner_create(machine, &input, debugger, agent);
    if (!runner) {
        fprintf(stderr, "velo: cannot start machine runner\n");
        return 1;
    }
    host_event_context_t host_events = {
        &machine, &key_layout, &input, &scroller, window, view, &running, &pen_down, held, &dropped, &picked, &roms,
        &session, &serial, desktop, &notices,
    };
    machine_menu_context_t machine_menu = {
        window, &machine, &settings, &profiles, &current, &current_index, profiles_folder, &snapshots, &notices, &session, &since_backup,
    };
    while (running) {
        uint64_t frame_start = SDL_GetTicksNS();
        app_runner_lock(runner);
        bool events_seen = poll_host_events(&host_events);

        release_keys(&input, machine, held, menu_modifiers());
        int switch_to = -1;
        for (int item = menu_poll(); item >= 0; item = menu_poll()) {
            release_keys(&input, machine, held, -1);
            if (handle_machine_menu(&machine_menu, item, &switch_to, &events_seen)) continue;
            if (handle_view_menu(&settings, window, view, item, &notices)) continue;
            switch (item) {
            case MENU_POWER:
                machine_power_button(machine, true);
                power_release_at = machine_cycles(machine) + (uint64_t)(POWER_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_PAUSE: paused = !paused; break;
            case MENU_SOFT_RESET:
                machine_soft_reset(machine);
                if (serial.mode == SERIAL_NETWORK) serial_unplug_at = machine_cycles(machine) + SOFT_RESET_REPLUG_SECONDS * MACHINE_CLOCK_HZ;
                break;
            case MENU_PASTE: {
                char *clipboard = SDL_GetClipboardText();
                size_t typed = clipboard ? typer_start(&typer, key_layout, clipboard) : 0;
                SDL_free(clipboard);
                snprintf(paste_notice, sizeof paste_notice, typed ? "typing %zu characters" : "nothing to type", typed);
                notice_queue_push(&notices, paste_notice);
                break;
            }
            case MENU_SAVE_STATE:
                notice_queue_push(&notices, machine_save(machine, session.state_path, (int64_t)time(NULL)) ? "state saved" : "could not save state");
                break;
            case MENU_LOAD_STATE:
                snapshot_store_backup_machine(&snapshots, machine, session.state_path);
                if (machine_load(machine, session.state_path, NULL)) {
                    serial_restored(&serial, machine, settings.serial_device, &serial_reconnect_at, &serial_reconnect_mode);
                    notice_queue_push(&notices, "state loaded");
                } else {
                    notice_queue_push(&notices, "no saved state");
                }
                break;
            case MENU_BACKLIGHT:
                machine_backlight_button(machine, true);
                backlight_release_at = machine_cycles(machine) + (uint64_t)(BACKLIGHT_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_SOUND: sound = !sound; break;
            case MENU_SHOW_STATE: reveal_file(session.state_path); break;
            case MENU_SAVE_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state" } };
                static char default_snapshot[1200];
                snapshot_store_default_name(&snapshots, default_snapshot, sizeof default_snapshot);
                SDL_ShowSaveFileDialog(pick_done, (void *)(intptr_t)PICK_SAVE_SNAPSHOT, window, filters, 1, default_snapshot);
                break;
            }
            case MENU_LOAD_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state;bin" } };
                static char folder[1100];
                snapshot_store_folder(&snapshots, folder, sizeof folder);
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_LOAD_SNAPSHOT, window, filters, 1, folder, false);
                break;
            }
            case MENU_SHOW_DEBUG_OUTPUT: {
                char path[1100];
                debug_log_path(path, sizeof path);
                if (debug_log) fflush(debug_log);
                open_path(path);
                break;
            }
            case MENU_QUIT:
                running = false;
                break;
#ifdef __ANDROID__
            case MENU_IMPORT: {
                int cards, imported = import_files(&cards);
                find_roms(&roms);
                char message[160];
                snprintf(message, sizeof message, "imported %d ROMs and %d cards", imported, cards);
                notice_queue_push(&notices, message);
                break;
            }
#endif
            case MENU_SPEED_1:
            case MENU_SPEED_2:
            case MENU_SPEED_4:
            case MENU_SPEED_8:
                settings.speed = item == MENU_SPEED_1 ? 1 : item == MENU_SPEED_2 ? 2 : item == MENU_SPEED_4 ? 4 : 8;
                machine_set_speed(machine, settings.speed);
                settings_save(&settings);
                break;
            case MENU_OPTIMISATIONS:
                settings.optimisations = !settings.optimisations;
                machine_set_optimisations(machine, settings.optimisations);
                settings_save(&settings);
                break;
            case MENU_INSERT_CARD: {
                static const SDL_DialogFileFilter filters[] = { { "Card images", "img;bin;raw" }, { "All files", "*" } };
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_CARD, window, filters, 2, NULL, false);
                break;
            }
            case MENU_INSERT_DISK: {
                static const SDL_DialogFileFilter filters[] = { { "Disk images", "img;bin;raw" }, { "All files", "*" } };
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_DISK, window, filters, 2, NULL, false);
                break;
            }
            case MENU_NEW_DISK: {
#ifdef __ANDROID__
                free(picked);
                picked = new_disk_pick();
                break;
#endif
                static const SDL_DialogFileFilter filters[] = { { "Disk images", "img" } };
                SDL_ShowSaveFileDialog(pick_done, (void *)(intptr_t)PICK_NEW_DISK, window, filters, 1, "Velo Disk.img");
                break;
            }
            case MENU_EJECT_DISK:
                machine_eject_disk(machine);
                notice_queue_push(&notices, "disk ejected");
                break;
            default:
                if (item >= MENU_MACHINE_FIRST && item <= MENU_MACHINE_LAST && item - MENU_MACHINE_FIRST < profiles.count && item - MENU_MACHINE_FIRST != current_index) {
                    switch_to = item - MENU_MACHINE_FIRST;
                    break;
                }
                if (item >= MENU_SERIAL_PORT_FIRST && item <= MENU_SERIAL_PORT_LAST && item - MENU_SERIAL_PORT_FIRST < port_count) {
                    snprintf(settings.serial_device, sizeof settings.serial_device, "%s", ports[item - MENU_SERIAL_PORT_FIRST]);
                    settings_save(&settings);
                    serial_reconnect_at = 0;
                    notice_queue_push(&notices, serial_open(&serial, machine, SERIAL_DEVICE, settings.serial_device));
                }
                break;
            case MENU_SERIAL_NETWORK:
            case MENU_SERIAL_PTY:
            case MENU_SERIAL_TCP:
            case MENU_SERIAL_OFF:
                serial_reconnect_at = 0;
                notice_queue_push(&notices,
                                  serial_open(&serial, machine, item == MENU_SERIAL_NETWORK ? SERIAL_NETWORK : item == MENU_SERIAL_PTY ? SERIAL_PTY : item == MENU_SERIAL_TCP ? SERIAL_TCP : SERIAL_OFF, settings.serial_device));
                break;
#ifdef __ANDROID__
            case MENU_FULL_BRIGHTNESS:
                settings.full_brightness = !settings.full_brightness;
                settings_save(&settings);
                break;
            case MENU_FETCH_DOCUMENTS:
            case MENU_SHARED_FOLDER: {
                if (!android_all_files_access()) {
                    android_request_all_files_access();
                    notice_queue_push(&notices, "allow All files access for Velo, then try again");
                    break;
                }
                bool shared = item == MENU_SHARED_FOLDER;
                picked_t *folder = calloc(1, sizeof *folder);
                const char *start = shared && settings.shared_folder[0] ? settings.shared_folder : "/storage/emulated/0/Documents";
                if (folder && android_choose_folder(shared ? "Folder to share with My Documents" : "Folder to copy My Documents into", start, folder->paths[0], sizeof folder->paths[0])) {
                    folder->kind = shared ? PICK_SHARED : PICK_FETCH;
                    folder->count = 1;
                    free(picked);
                    picked = folder;
                } else {
                    free(folder);
                }
                events_seen = true;
                break;
            }
#endif
            case MENU_GDB_SERVER:
                if (debugger) {
                    app_runner_set_debugger_locked(runner, NULL);
                    gdb_destroy(debugger);
                    debugger = NULL;
                    settings.gdb_server = 0;
                    notice_queue_push(&notices, "GDB server stopped");
                } else {
                    debugger = start_network_gdb(machine, settings.gdb_port, gdb_notice, sizeof gdb_notice);
                    app_runner_set_debugger_locked(runner, debugger);
                    settings.gdb_server = debugger != NULL;
                    notice_queue_push(&notices, gdb_notice);
                }
                settings_save(&settings);
                break;
            case MENU_NETWORK_RAPI: {
                settings.network_rapi = !settings.network_rapi;
                settings_save(&settings);
                serial.options.rapi_port = settings.network_rapi ? (int)settings.rapi_port : 0;
                char address[64];
                local_address(address, sizeof address);
                char message[160];
                if (settings.network_rapi) snprintf(message, sizeof message, "RAPI at %s:%u", address, settings.rapi_port);
                else snprintf(message, sizeof message, "RAPI over the network off");
                if (serial.mode == SERIAL_NETWORK) serial_open(&serial, machine, SERIAL_NETWORK, settings.serial_device);
                notice_queue_push(&notices, message);
                break;
            }
            case MENU_SEND_FILES:
                SDL_ShowOpenFileDialog(pick_done, (void *)(intptr_t)PICK_SEND, window, NULL, 0, NULL, true);
                break;
#ifndef __ANDROID__
            case MENU_FETCH_DOCUMENTS:
                SDL_ShowOpenFolderDialog(pick_done, (void *)(intptr_t)PICK_FETCH, window, NULL, false);
                break;
            case MENU_SHARED_FOLDER:
                SDL_ShowOpenFolderDialog(pick_done, (void *)(intptr_t)PICK_SHARED, window, settings.shared_folder[0] ? settings.shared_folder : NULL, false);
                break;
#endif
            case MENU_SYNC_NOW:
                desktop_sync(desktop, settings.shared_folder);
                break;
            case MENU_SET_PROXY:
                desktop_set_proxy(desktop);
                break;
            case MENU_BAUD_19200:
            case MENU_BAUD_38400:
            case MENU_BAUD_57600:
            case MENU_BAUD_115200:
                desktop_set_baud(desktop, item == MENU_BAUD_19200 ? 19200 : item == MENU_BAUD_38400 ? 38400 : item == MENU_BAUD_57600 ? 57600 : 115200);
                break;
            case MENU_STOP_SHARING:
                snprintf(shared_notice, sizeof shared_notice, "stopped sharing %s", file_leaf_name(settings.shared_folder));
                settings.shared_folder[0] = 0;
                settings_save(&settings);
                notice_queue_push(&notices, shared_notice);
                break;
            case MENU_EJECT_CARD:
                machine_eject_card(machine);
                notice_queue_push(&notices, "card ejected");
                break;
            }
        }
        if (switch_to >= 0 && switch_to < profiles.count && switch_to != current_index) {
            if (desktop_busy(desktop)) {
                notice_queue_push(&notices, "busy with a desktop transfer");
            } else {
                const char *switch_notice = NULL;
                machine_session_t next_session = { 0 };
                profile_t next_profile = profiles.entries[switch_to];
                bool next_started = machine_session_start(&next_session, &next_profile, settings.speed, settings.optimisations != 0,
                                                          NULL, false, &switch_notice, &snapshots, &session_hooks);
                if (!next_started) {
                    notice_queue_push(&notices, switch_notice);
                } else {
                    serial_mode_t mode = serial.mode;
                    if (serial_keeps_link(&serial, mode, settings.serial_device)) machine_serial_connect(machine, false);
                    else serial_close(&serial, machine);
                    if (pen_down) machine_touch(machine, false, 0, 0);
                    input_clear(&input);
                    pen_down = false;
                    typer.length = typer.position = 0;
                    machine_save(machine, session.state_path, (int64_t)time(NULL));
                    machine_session_destroy(&session);
                    session = next_session;
                    machine = session.machine;
                    current = next_profile;
                    current_index = switch_to;
                    key_layout = machine_key_layout(machine);
                    snprintf(settings.machine, sizeof settings.machine, "%s", current.id);
                    settings_save(&settings);
                    serial_reconnect_at = serial_unplug_at = 0;
                    if (mode != SERIAL_OFF) {
                        serial_reconnect_mode = mode;
                        serial_reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
                    }
                    power_release_at = backlight_release_at = 0;
                    since_backup = 0;
                    app_runner_set_machine_locked(runner, machine);
                    if (debugger) gdb_set_machine(debugger, machine);
                    char message[160];
                    snprintf(message, sizeof message, "switched to %s", current.name);
                    notice_queue_push(&notices, switch_notice ? switch_notice : message);
                }
            }
        }
        bool velo_online = serial.gateway && net_gateway_online(serial.gateway);
        if (picked) {
            if (picked->kind == PICK_CARD) {
                notice_queue_push(&notices, machine_insert_card(machine, picked->paths[0]) ? "card inserted" : "could not open card image");
            } else if (picked->kind == PICK_DISK) {
                notice_queue_push(&notices, machine_insert_disk(machine, picked->paths[0], false) ? "disk inserted" : "could not open disk image");
            } else if (picked->kind == PICK_NEW_DISK) {
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked->paths[0], has_extension(picked->paths[0], ".img") ? "" : ".img");
                bool made = create_blank_disk(path) && machine_insert_disk(machine, path, false);
                char message[1200];
                snprintf(message, sizeof message, made ? "inserted new disk %s; the Velo offers to format it" : "could not create %s", file_leaf_name(path));
                notice_queue_push(&notices, message);
            } else if (picked->kind == PICK_SEND) {
                const char *files[PICK_MAX + 1];
                for (int i = 0; i < picked->count; i++) files[i] = picked->paths[i];
                files[picked->count] = NULL;
                desktop_send(desktop, files);
            } else if (picked->kind == PICK_SAVE_SNAPSHOT) {
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked->paths[0], has_extension(picked->paths[0], ".state") ? "" : ".state");
                bool saved = machine_save(machine, path, (int64_t)time(NULL));
#ifdef __ANDROID__
                if (saved && picked->export_uri[0]) {
                    saved = android_export(path, picked->export_uri);
                    remove(path);
                }
#endif
                char message[1200];
                snprintf(message, sizeof message, saved ? "saved snapshot %s" : "could not save %s", file_leaf_name(path));
                notice_queue_push(&notices, message);
            } else if (picked->kind == PICK_LOAD_SNAPSHOT) {
                char message[1200];
                if (machine_state_matches(machine, picked->paths[0])) snapshot_store_backup_machine(&snapshots, machine, session.state_path);
                if (machine_load(machine, picked->paths[0], NULL)) {
                    serial_restored(&serial, machine, settings.serial_device, &serial_reconnect_at, &serial_reconnect_mode);
                    snprintf(message, sizeof message, "loaded snapshot %s", file_leaf_name(picked->paths[0]));
                } else {
                    snprintf(message, sizeof message, "%s isn't a snapshot of this ROM", file_leaf_name(picked->paths[0]));
                }
                notice_queue_push(&notices, message);
            } else if (picked->kind == PICK_FETCH) {
                desktop_fetch(desktop, picked->paths[0]);
            } else if (picked->kind == PICK_SHARED) {
                snprintf(settings.shared_folder, sizeof settings.shared_folder, "%s", picked->paths[0]);
                settings_save(&settings);
                snprintf(shared_notice, sizeof shared_notice, "sharing %s with \\My Documents", file_leaf_name(settings.shared_folder));
                notice_queue_push(&notices, shared_notice);
                if (velo_online) desktop_sync(desktop, settings.shared_folder);
            }
            free(picked);
            picked = NULL;
        }
        if (serial.gateway && net_gateway_take_desktop_connected(serial.gateway) && settings.shared_folder[0]) {
            desktop_sync(desktop, settings.shared_folder);
        }
        if (desktop_take_reconnect(desktop) && serial.mode == SERIAL_NETWORK) serial_unplug_at = machine_cycles(machine) + SPEED_SETTLE_SECONDS * MACHINE_CLOCK_HZ;
        if (serial_unplug_at && machine_cycles(machine) >= serial_unplug_at) {
            serial_unplug_at = 0;
            if (serial.mode == SERIAL_NETWORK) {
                serial_open(&serial, machine, SERIAL_OFF, settings.serial_device);
                serial_reconnect_mode = SERIAL_NETWORK;
                serial_reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
            }
        }
        if (desktop_take_status(desktop, desktop_notice, sizeof desktop_notice)) {
            notice_queue_push(&notices, desktop_notice);
        }
        bool desktop_free = velo_online && !desktop_busy(desktop);
        menu_ensure();
        menu_set_enabled(MENU_SEND_FILES, desktop_free);
        menu_set_enabled(MENU_FETCH_DOCUMENTS, desktop_free);
        menu_set_enabled(MENU_SYNC_NOW, desktop_free && settings.shared_folder[0]);
        menu_set_enabled(MENU_STOP_SHARING, settings.shared_folder[0] != 0);
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
        menu_set_checked(MENU_SERIAL_NETWORK, serial.mode == SERIAL_NETWORK);
        menu_set_checked(MENU_SERIAL_PTY, serial.mode == SERIAL_PTY);
        menu_set_checked(MENU_SERIAL_TCP, serial.mode == SERIAL_TCP);
        reap_reveal_children();
        if (since_port_scan <= 0) {
            since_port_scan = PORT_SCAN_SECONDS;
            port_count = serial_link_ports(ports, SERIAL_PORT_MAX);
        }
        for (int i = 0; i < SERIAL_PORT_MAX; i++) {
            int port_item = MENU_SERIAL_PORT_FIRST + i;
            bool shown = i < port_count || (i == 0 && port_count == 0);
            menu_set_hidden(port_item, !shown);
            if (!shown) continue;
            menu_set_title(port_item, port_count ? ports[i] + 5 : "No serial ports found");
            menu_set_enabled(port_item, port_count > 0);
            menu_set_checked(port_item, port_count && serial.mode == SERIAL_DEVICE && !strcmp(settings.serial_device, ports[i]));
        }
        menu_set_checked(MENU_SERIAL_OFF, serial.mode == SERIAL_OFF);
        menu_set_checked(MENU_PAUSE, paused);
        menu_set_checked(MENU_BACKLIGHT, machine_backlight(machine));
        menu_set_checked(MENU_SOUND, sound);
        menu_set_checked(MENU_GDB_SERVER, debugger != NULL);
        menu_set_checked(MENU_NETWORK_RAPI, settings.network_rapi != 0);
        menu_set_checked(MENU_FULL_BRIGHTNESS, settings.full_brightness != 0);
        for (int i = 0; i < PROFILES_MAX; i++) {
            int machine_item = MENU_MACHINE_FIRST + i;
            menu_set_hidden(machine_item, i >= profiles.count);
            if (i >= profiles.count) continue;
            menu_set_title(machine_item, profiles.entries[i].name);
            menu_set_checked(machine_item, i == current_index);
        }
        menu_set_enabled(MENU_NEW_MACHINE, profiles.count < PROFILES_MAX);
        menu_set_checked(MENU_CONNECT_AT_LAUNCH, settings.connect_at_launch != 0);
        for (int scale_item = MENU_SCALE_50; scale_item <= MENU_SCALE_200; scale_item++) menu_set_checked(scale_item, settings.scale == settings_scale_at(scale_item - MENU_SCALE_50));
        menu_set_enabled(MENU_ZOOM_IN, settings.scale < settings_scale_at(SETTINGS_SCALE_COUNT - 1));
        menu_set_enabled(MENU_ZOOM_OUT, settings.scale > settings_scale_at(0));
        menu_set_checked(MENU_FULL_SCREEN, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
        menu_set_checked(MENU_DISPLAY_SIMULATED, settings.display == VIEW_SIMULATED);
        menu_set_checked(MENU_DISPLAY_SHARP, settings.display == VIEW_SHARP);
        menu_set_checked(MENU_SPEED_1, machine_speed(machine) == 1);
        menu_set_checked(MENU_SPEED_2, machine_speed(machine) == 2);
        menu_set_checked(MENU_SPEED_4, machine_speed(machine) == 4);
        menu_set_checked(MENU_SPEED_8, machine_speed(machine) == 8);
        menu_set_checked(MENU_OPTIMISATIONS, machine_optimisations(machine));

        uint64_t now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - last) / frequency;
        last = now;
        if (elapsed > MAX_FRAME_SLICE) elapsed = MAX_FRAME_SLICE;
        const char *notice = notice_queue_current(&notices);
        set_title(window, current.name, notice, paused, machine_suspended(machine));
#ifdef __ANDROID__
        if (android_toast(notice ? notice : paused ? "Paused" : NULL)) events_seen = true;
#endif
        since_autosave += elapsed;
        if (!paused) since_backup += elapsed;
        since_port_scan -= elapsed;
        if (since_backup >= BACKUP_SECONDS) {
            since_backup = 0;
            snapshot_store_backup_machine(&snapshots, machine, session.state_path);
        }
        if (since_autosave >= AUTOSAVE_SECONDS) {
            since_autosave = 0;
            machine_save(machine, session.state_path, (int64_t)time(NULL));
        }
        app_runner_set_paused_locked(runner, paused);
        typer_step(&typer, machine);
        scroller_step(&scroller, machine);
        serial_link_pump(&serial, machine);
        if (serial.mode != SERIAL_TCP) {
            serial_tcp_attached = false;
        } else if (serial_link_attached(&serial) != serial_tcp_attached) {
            serial_tcp_attached = !serial_tcp_attached;
            machine_serial_connect(machine, serial_tcp_attached);
            notice_queue_push(&notices, serial_tcp_attached ? "TCP client connected" : "TCP client disconnected");
        }
        if (serial_reconnect_at && machine_cycles(machine) >= serial_reconnect_at) {
            serial_reconnect_at = 0;
            notice_queue_push(&notices, serial_open(&serial, machine, serial_reconnect_mode, settings.serial_device));
        }
        if (backlight_release_at && machine_cycles(machine) >= backlight_release_at) {
            backlight_release_at = 0;
            machine_backlight_button(machine, false);
        }
        if (power_release_at && machine_cycles(machine) >= power_release_at) {
            power_release_at = 0;
            machine_power_button(machine, false);
        }
        uint32_t rate;
        for (size_t count; (count = machine_audio(machine, samples, AUDIO_CHUNK, &rate)) > 0;) {
            if (!audio || !sound) continue;
            if ((int)rate != audio_spec.freq) {
                audio_spec.freq = (int)rate;
                SDL_SetAudioStreamFormat(audio, &audio_spec, NULL);
            }
            SDL_PutAudioStreamData(audio, samples, (int)(count * sizeof samples[0]));
        }

        screen = machine_screen_size(machine);
        if (screen.width != lcd_width() || screen.height != lcd_height()) {
            view_set_screen_size(view, screen.width, screen.height);
            if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) fit_window(window, view, settings.scale);
        }
        lcd_set_power(machine_lcd_enabled(machine));
        lcd_set_backlight(machine_backlight(machine));
#ifdef __ANDROID__
        android_update(settings.full_brightness && machine_backlight(machine) && machine_lcd_enabled(machine), !machine_suspended(machine));
#endif
        machine_screen(machine, lcd_framebuffer);
        bool lcd_on = machine_lcd_enabled(machine);
        app_runner_unlock(runner);
        int inset_left, inset_top, inset_right, inset_bottom;
        menu_insets(&inset_left, &inset_top, &inset_right, &inset_bottom);
        view_set_insets(view, inset_left, inset_top, inset_right, inset_bottom);
        bool screen_changed = view_update(view, (float)elapsed, lcd_on);
        if (screen_changed || events_seen || menu_active()) {
            view_render(view);
            menu_draw(renderer);
            SDL_RenderPresent(renderer);
        } else {
            uint64_t spent = SDL_GetTicksNS() - frame_start;
            if (spent < IDLE_FRAME_NS) SDL_DelayNS(IDLE_FRAME_NS - spent);
        }
    }

    app_runner_destroy(runner);
    gdb_destroy(debugger);
    debugger = NULL;
    agent_destroy(agent);
    agent = NULL;
    free(picked);
    machine_save(machine, session.state_path, (int64_t)time(NULL));
    serial_close(&serial, machine);
    desktop_destroy(desktop);
    if (verbose) machine_dump_state(machine);
    SDL_DestroyAudioStream(audio);
    view_destroy(view);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    machine_session_destroy(&session);
    return 0;
}

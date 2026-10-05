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
#include "app/menu.h"
#include "app/profiles.h"
#include "app/typer.h"
#include "app/view.h"
#include "core/agent.h"
#include "core/gdb.h"
#include "core/key_text.h"
#include "core/lcd.h"
#include "core/machine.h"
#include "net/net_gateway.h"
#include "rapi/rapi.h"
#include "util/fat.h"
#include "util/file.h"
#include "util/marker.h"
#include "util/options.h"
#include "util/png.h"

#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <termios.h>
#include <unistd.h>

#define WINDOW_SCALE     2
#define IDLE_FRAME_NS    (SDL_NS_PER_SECOND / 60)
#define RUN_HOLD_NS      (4 * SDL_NS_PER_MS)
#define RUN_SLICE_CYCLES (MACHINE_CLOCK_HZ / 1000)
#define RUN_MAX_BEHIND   (MACHINE_CLOCK_HZ / 10)
#define MAX_FRAME_SLICE  0.1
#define AUTOSAVE_SECONDS 60
#define BACKUP_SECONDS   600
#define BACKUP_KEEP      10
#define NOTICE_SECONDS   2
#define SPEED_SETTLE_SECONDS 10ull
#define POWER_PRESS_SECONDS 0.2
#define BACKLIGHT_PRESS_SECONDS 0.1
#define AUDIO_CHUNK 8192
#define WINDOW_TITLE     "Philips Velo 1"
#define ANDROID_UNLIT_LEVEL 0.5f
#define CE2_DEFAULT_MEMORY 32

#ifdef __APPLE__
#define SCREENSHOT_FOLDER SDL_FOLDER_DESKTOP
#else
#define SCREENSHOT_FOLDER SDL_FOLDER_PICTURES
#endif

typedef struct {
    SDL_Keycode key;
    uint8_t     scancode;
} key_binding_t;

static const key_binding_t key_bindings[] = {
    { SDLK_TAB, 0x11 }, { SDLK_BACKSPACE, 0x39 }, { SDLK_RETURN, 0x4B },
    { SDLK_ESCAPE, 0x29 }, { SDLK_LSHIFT, 0x51 }, { SDLK_RSHIFT, 0x51 }, { SDLK_LCTRL, 0x01 },
    { SDLK_RCTRL, 0x01 }, { SDLK_LALT, 0x19 }, { SDLK_RALT, 0x09 },
    { SDLK_LEFT, 0x41 }, { SDLK_UP, 0x4A }, { SDLK_RIGHT, 0x32 }, { SDLK_DOWN, 0x49 },
};

static bool find_scancode(key_layout_t layout, SDL_Keycode key, uint8_t *scancode) {
    if (key >= 0x20 && key < 0x7F) {
        bool shifted;
        return key_text_find(layout, (char)key, scancode, &shifted);
    }
    for (size_t i = 0; i < sizeof key_bindings / sizeof key_bindings[0]; i++) {
        if (key_bindings[i].key == key) { *scancode = key_bindings[i].scancode; return true; }
    }
    return false;
}

static bool verbose = false;

#define SCROLL_STEP    (MACHINE_CLOCK_HZ / 50)
#define SCROLL_PENDING 8

typedef struct {
    float    vertical, horizontal;
    int      pending;
    uint8_t  scancode;
    bool     pressed;
    uint64_t next_at;
} scroller_t;

static void scroller_add(scroller_t *scroller, float vertical, float horizontal) {
    scroller->vertical += vertical;
    scroller->horizontal += horizontal;
    float *axis = fabsf(scroller->vertical) >= fabsf(scroller->horizontal) ? &scroller->vertical : &scroller->horizontal;
    if (fabsf(*axis) < 1.0f) return;
    uint8_t scancode = axis == &scroller->vertical ? (*axis > 0 ? 0x4A : 0x49) : (*axis > 0 ? 0x32 : 0x41);
    int steps = (int)fabsf(*axis);
    *axis -= *axis > 0 ? (float)steps : -(float)steps;
    if (scancode != scroller->scancode) {
        if (scroller->pressed) return;
        scroller->scancode = scancode;
        scroller->pending = 0;
    }
    scroller->pending += steps;
    if (scroller->pending > SCROLL_PENDING) scroller->pending = SCROLL_PENDING;
}

static void scroller_step(scroller_t *scroller, machine_t *machine) {
    uint64_t now = machine_cycles(machine);
    if (scroller->next_at > now + SCROLL_STEP) scroller->next_at = now;
    if (!scroller->pending || now < scroller->next_at) return;
    machine_key(machine, scroller->scancode, scroller->pressed);
    if (scroller->pressed) scroller->pending--;
    scroller->pressed = !scroller->pressed;
    scroller->next_at = now + SCROLL_STEP;
}

#define INPUT_QUEUE    64
#define PEN_MIN_CYCLES (MACHINE_CLOCK_HZ * 6 / 100)
#define KEY_MIN_CYCLES (MACHINE_CLOCK_HZ / 50)

typedef enum { INPUT_PEN, INPUT_KEY } input_kind_t;

typedef struct {
    struct { uint64_t at; input_kind_t kind; bool down; int x, y; uint8_t scancode; } events[INPUT_QUEUE];
    int      count;
    uint64_t last_at, seen;
} input_queue_t;

static void input_rebase(input_queue_t *input, uint64_t now) {
    if (now < input->seen) {
        for (int i = 0; i < input->count; i++) input->events[i].at = now;
        input->last_at = now;
    }
    input->seen = now;
}

static void input_add(input_queue_t *input, machine_t *machine, input_kind_t kind, bool down, int x, int y, uint8_t scancode) {
    if (input->count == INPUT_QUEUE) return;
    uint64_t now = machine_cycles(machine);
    input_rebase(input, now);
    uint64_t spacing = kind == INPUT_PEN ? PEN_MIN_CYCLES : KEY_MIN_CYCLES;
    uint64_t at = input->last_at + spacing > now ? input->last_at + spacing : now;
    input->events[input->count].at = at;
    input->events[input->count].kind = kind;
    input->events[input->count].down = down;
    input->events[input->count].x = x;
    input->events[input->count].y = y;
    input->events[input->count].scancode = scancode;
    input->count++;
    input->last_at = at;
}

static void pen_move(input_queue_t *input, machine_t *machine, int x, int y) {
    if (input->count) return;
    machine_touch(machine, true, x, y);
}

static void input_step(input_queue_t *input, machine_t *machine) {
    uint64_t now = machine_cycles(machine);
    input_rebase(input, now);
    int done = 0;
    while (done < input->count && input->events[done].at <= now) {
        if (input->events[done].kind == INPUT_PEN) machine_touch(machine, input->events[done].down, input->events[done].x, input->events[done].y);
        else machine_key(machine, input->events[done].scancode, !input->events[done].down);
        done++;
    }
    for (int i = done; i < input->count; i++) input->events[i - done] = input->events[i];
    input->count -= done;
}

static void input_clear(input_queue_t *input) {
    input->count = 0;
    input->last_at = input->seen = 0;
}

static gdb_t *debugger;
static agent_t *agent;

static void log_gdb(const char *message) {
    fputs(message, stderr);
}

typedef struct {
    SDL_Mutex  *lock;
    SDL_Thread *thread;
    machine_t  *machine;
    input_queue_t *input;
    bool        paused, stop, restart;
    SDL_AtomicInt waiting;
} runner_t;

static int run_machine(void *context) {
    runner_t *runner = context;
    double owed = 0;
    uint64_t last = SDL_GetTicksNS();
    for (;;) {
        SDL_LockMutex(runner->lock);
        if (runner->stop) {
            SDL_UnlockMutex(runner->lock);
            return 0;
        }
        uint64_t now = SDL_GetTicksNS();
        if (debugger) gdb_service(debugger);
        if (runner->restart || runner->paused || (debugger && gdb_halted(debugger))) {
            owed = 0;
            runner->restart = false;
        } else {
            owed += (double)(now - last) * MACHINE_CLOCK_HZ / SDL_NS_PER_SECOND;
            if (owed > RUN_MAX_BEHIND) owed = RUN_MAX_BEHIND;
            uint64_t hold_until = now + RUN_HOLD_NS;
            while (owed >= RUN_SLICE_CYCLES && SDL_GetTicksNS() < hold_until && !SDL_GetAtomicInt(&runner->waiting)) {
                input_step(runner->input, runner->machine);
                machine_run(runner->machine, RUN_SLICE_CYCLES);
                owed -= RUN_SLICE_CYCLES;
                if (agent) agent_poll(agent, machine_mailbox(runner->machine));
                if (!debugger) continue;
                gdb_after_run(debugger);
                if (gdb_halted(debugger)) break;
            }
        }
        last = now;
        bool caught_up = owed < RUN_SLICE_CYCLES;
        SDL_UnlockMutex(runner->lock);
        if (caught_up) SDL_DelayNS(SDL_NS_PER_MS / 2);
        while (SDL_GetAtomicInt(&runner->waiting)) SDL_DelayNS(SDL_NS_PER_MS / 10);
    }
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

typedef enum { SERIAL_OFF, SERIAL_NETWORK, SERIAL_PTY, SERIAL_DEVICE } serial_mode_t;

#define SERIAL_QUEUE 65536

typedef struct {
    serial_mode_t mode;
    net_gateway_t *gateway;
    int      pty;
    int      pty_slave;
    char     pty_name[128];
    const char *user_agent;
    const char *device;
    uint32_t baud;
    uint8_t  queue[SERIAL_QUEUE];
    size_t   queued;
} serial_t;

#define SERIAL_PORT_MAX   16
#define PORT_SCAN_SECONDS 2.0

static bool is_serial_port(const char *name) {
    return !strncmp(name, "cu.", 3) || !strncmp(name, "ttyUSB", 6) || !strncmp(name, "ttyACM", 6);
}

static int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

static int list_serial_ports(char ports[][64], int max) {
#ifdef __ANDROID__
    (void)ports;
    (void)max;
    return 0;
#endif
    DIR *dev = opendir("/dev");
    if (!dev) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dev)) && count < max) {
        if (!is_serial_port(entry->d_name) || strlen(entry->d_name) + 6 > 64) continue;
        snprintf(ports[count++], 64, "/dev/%s", entry->d_name);
    }
    closedir(dev);
    qsort(ports, (size_t)count, 64, compare_names);
    return count;
}

static speed_t speed_for(uint32_t baud) {
    static const struct { uint32_t baud; speed_t speed; } speeds[] = {
        { 300, B300 }, { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
        { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
    };
    speed_t best = B9600;
    uint32_t best_error = UINT32_MAX;
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
        uint32_t error = speeds[i].baud > baud ? speeds[i].baud - baud : baud - speeds[i].baud;
        if (error < best_error) { best_error = error; best = speeds[i].speed; }
    }
    return best;
}

static void device_follow_baud(serial_t *serial, machine_t *machine) {
    uint32_t baud = machine_serial_baud(machine);
    if (serial->mode != SERIAL_DEVICE || !baud || baud == serial->baud) return;
    struct termios settings;
    if (tcgetattr(serial->pty, &settings) != 0) return;
    cfsetispeed(&settings, speed_for(baud));
    cfsetospeed(&settings, speed_for(baud));
    tcsetattr(serial->pty, TCSANOW, &settings);
    serial->baud = baud;
    if (verbose) fprintf(stderr, "serial: %s at %u baud\n", serial->device, baud);
}

static void serial_log(const char *message) {
    if (verbose) fputs(message, stderr);
}

static void serial_close(serial_t *serial, machine_t *machine) {
    if (serial->gateway) net_gateway_destroy(serial->gateway);
    if (serial->pty >= 0) close(serial->pty);
    if (serial->pty_slave >= 0) close(serial->pty_slave);
    serial->gateway = NULL;
    serial->pty = -1;
    serial->pty_slave = -1;
    serial->queued = 0;
    serial->baud = 0;
    serial->mode = SERIAL_OFF;
    machine_serial_connect(machine, false);
}

static void rapi_socket_path(char *path, size_t size) {
#ifdef __ANDROID__
    net_gateway_socket_path(path, size, "velo-rapi");
#else
    rapi_data_path("rapi.sock", path, size);
#endif
}

static const char *serial_open(serial_t *serial, machine_t *machine, serial_mode_t mode) {
    serial_close(serial, machine);
    if (mode == SERIAL_NETWORK) {
        char rapi_socket[1024];
        rapi_socket_path(rapi_socket, sizeof rapi_socket);
        net_gateway_options_t options = { serial->user_agent, rapi_socket };
        serial->gateway = net_gateway_create(serial_log, &options);
        if (!serial->gateway) return "built without libslirp";
    } else if (mode == SERIAL_PTY) {
        int fd = posix_openpt(O_RDWR | O_NOCTTY);
        if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
            if (fd >= 0) close(fd);
            return "could not open a pseudo-terminal";
        }
        snprintf(serial->pty_name, sizeof serial->pty_name, "%s", ptsname(fd));
        int slave = open(serial->pty_name, O_RDWR | O_NOCTTY);
        struct termios settings;
        tcgetattr(slave, &settings);
        cfmakeraw(&settings);
        tcsetattr(slave, TCSANOW, &settings);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        serial->pty = fd;
        serial->pty_slave = slave;
        fprintf(stderr, "serial: COM1 on %s\n", serial->pty_name);
    } else if (mode == SERIAL_DEVICE) {
        if (!serial->device || !serial->device[0]) return "choose a host serial port first";
        int fd = open(serial->device, O_RDWR | O_NOCTTY | O_NONBLOCK);
        struct termios settings;
        if (fd < 0 || tcgetattr(fd, &settings) != 0) {
            if (fd >= 0) close(fd);
            snprintf(serial->pty_name, sizeof serial->pty_name, "could not open %s", serial->device);
            return serial->pty_name;
        }
        cfmakeraw(&settings);
        settings.c_cflag |= CLOCAL | CREAD;
        settings.c_cflag &= ~(tcflag_t)CRTSCTS;
        cfsetispeed(&settings, B19200);
        cfsetospeed(&settings, B19200);
        tcsetattr(fd, TCSANOW, &settings);
        serial->pty = fd;
        snprintf(serial->pty_name, sizeof serial->pty_name, "COM1 on %s", serial->device);
    }
    serial->mode = mode;
    machine_set_serial_tag(machine, (uint32_t)mode);
    if (mode != SERIAL_OFF) machine_serial_connect(machine, true);
    if (mode == SERIAL_DEVICE) device_follow_baud(serial, machine);
    return mode == SERIAL_NETWORK ? "network cable connected" : mode != SERIAL_OFF ? serial->pty_name : "serial disconnected";
}

static void serial_restored(serial_t *serial, machine_t *machine, uint64_t *reconnect_at, serial_mode_t *reconnect_mode) {
    bool was_connected = machine_serial_connected(machine);
    serial_mode_t mode = (serial_mode_t)machine_serial_tag(machine);
    serial_close(serial, machine);
    *reconnect_at = 0;
    if (was_connected && (mode == SERIAL_NETWORK || mode == SERIAL_PTY || (mode == SERIAL_DEVICE && serial->device && serial->device[0]))) {
        *reconnect_mode = mode;
        *reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
    }
}

static void serial_pump(serial_t *serial, machine_t *machine) {
    uint8_t buffer[4096];
    size_t count;
    device_follow_baud(serial, machine);
    while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) {
        if (serial->gateway) net_gateway_from_guest(serial->gateway, buffer, count);
        else if (serial->mode == SERIAL_DEVICE) {
            size_t room = sizeof serial->queue - serial->queued;
            if (count > room) count = room;
            memcpy(serial->queue + serial->queued, buffer, count);
            serial->queued += count;
        }
        else if (serial->pty >= 0 && write(serial->pty, buffer, count) < 0) break;
    }
    if (serial->mode == SERIAL_DEVICE && serial->queued) {
        ssize_t written = write(serial->pty, serial->queue, serial->queued);
        if (written > 0) {
            memmove(serial->queue, serial->queue + written, serial->queued - (size_t)written);
            serial->queued -= (size_t)written;
        }
    }
    if (serial->gateway) {
        net_gateway_poll(serial->gateway, machine_cycles(machine) / (MACHINE_CLOCK_HZ / 1000));
        while ((count = net_gateway_to_guest(serial->gateway, buffer, sizeof buffer)) > 0) machine_serial_send(machine, buffer, count);
    } else if (serial->pty >= 0) {
        ssize_t got;
        while ((got = read(serial->pty, buffer, sizeof buffer)) > 0) machine_serial_send(machine, buffer, (size_t)got);
    }
}
typedef enum { PICK_SEND = 1, PICK_FETCH, PICK_SHARED, PICK_SAVE_SNAPSHOT, PICK_LOAD_SNAPSHOT, PICK_CARD, PICK_DISK, PICK_NEW_DISK } pick_kind_t;

#define PICK_MAX 64

typedef struct {
    pick_kind_t kind;
    int         count;
    char        paths[PICK_MAX][1024];
    char        export_uri[1024];
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
    int  count;
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
    if (verbose) fputs(message, stderr);
}

#define DEBUG_LOG_MAX (1024 * 1024)

static FILE *debug_log;
static bool  debug_to_stderr;

static void data_folder(char *path, size_t size);

static void debug_log_path(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
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

static void data_folder(char *path, size_t size) {
    rapi_data_path("", path, size);
    size_t length = strlen(path);
    if (length > 1 && path[length - 1] == '/') path[length - 1] = 0;
    SDL_CreateDirectory(path);
}

static void migrate_old_folders(void) {
#ifdef __APPLE__
    if (getenv("XDG_DATA_HOME") || getenv("XDG_CONFIG_HOME")) return;
    const char *home = getenv("HOME");
    if (!home) return;
    char folder[1024], old_data[1024], old_settings[1024], settings[1100];
    rapi_data_path("", folder, sizeof folder);
    folder[strlen(folder) - 1] = 0;
    snprintf(old_data, sizeof old_data, "%s/.local/share/velo-emu", home);
    snprintf(old_settings, sizeof old_settings, "%s/.config/velo-emu/emu.ini", home);
    struct stat info;
    if (stat(folder, &info) != 0 && stat(old_data, &info) == 0) {
        char parent[1100];
        snprintf(parent, sizeof parent, "%s/Library/Application Support", home);
        SDL_CreateDirectory(parent);
        if (rename(old_data, folder) == 0) fprintf(stderr, "moved %s to %s\n", old_data, folder);
    }
    SDL_CreateDirectory(folder);
    snprintf(settings, sizeof settings, "%s/emu.ini", folder);
    if (stat(settings, &info) != 0 && stat(old_settings, &info) == 0 && rename(old_settings, settings) == 0) {
        fprintf(stderr, "moved %s to %s\n", old_settings, settings);
    }
#endif
}

#ifdef __ANDROID__
static void localize_picked(picked_t *picked) {
    if (picked->kind == PICK_NEW_DISK) return;
    char folder[1100];
    if (picked->kind == PICK_CARD || picked->kind == PICK_DISK) {
        char base[1024];
        data_folder(base, sizeof base);
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
    data_folder(base, sizeof base);
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

static void snapshot_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/snapshots", base);
    SDL_CreateDirectory(path);
}

static void backup_path(const char *state, char *prefix, size_t prefix_size, char *path, size_t size) {
    char folder[1100];
    snapshot_folder(folder, sizeof folder);
    snprintf(folder + strlen(folder), sizeof folder - strlen(folder), "/Backups");
    SDL_CreateDirectory(folder);
    char name[256];
    snprintf(name, sizeof name, "%s", file_leaf_name(state));
    char *extension = strrchr(name, '.');
    if (extension && extension != name) *extension = 0;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(prefix, prefix_size, "%s ", name);
    snprintf(path, size, "%s/%s%s.state", folder, prefix, stamp);
}

static int compare_name_pointers(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void prune_backups(const char *path, const char *prefix) {
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (!slash) return;
    *slash = 0;
    DIR *dir = opendir(folder);
    if (!dir) return;
    char *names[256];
    int count = 0;
    size_t prefix_length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < 256) {
        if (strncmp(entry->d_name, prefix, prefix_length) || !has_extension(entry->d_name, ".state")) continue;
        names[count] = strdup(entry->d_name);
        if (names[count]) count++;
    }
    closedir(dir);
    qsort(names, (size_t)count, sizeof names[0], compare_name_pointers);
    for (int i = 0; i < count; i++) {
        if (i < count - BACKUP_KEEP) {
            char old[1400];
            snprintf(old, sizeof old, "%s/%s", folder, names[i]);
            remove(old);
        }
        free(names[i]);
    }
}

static bool backup_machine(machine_t *machine, const char *state) {
    char prefix[300], path[1400];
    backup_path(state, prefix, sizeof prefix, path, sizeof path);
    bool saved = machine_save(machine, path, (int64_t)time(NULL));
    if (saved) prune_backups(path, prefix);
    return saved;
}

static bool backup_file(const char *state) {
    size_t size;
    uint8_t *contents = file_read(state, &size);
    if (!contents) return false;
    char prefix[300], path[1400];
    backup_path(state, prefix, sizeof prefix, path, sizeof path);
    FILE *file = fopen(path, "wb");
    bool written = file && fwrite(contents, 1, size, file) == size;
    if (file && fclose(file) != 0) written = false;
    free(contents);
    if (written) prune_backups(path, prefix);
    return written;
}

static void snapshot_default_name(char *path, size_t size) {
    char folder[1100];
    snapshot_folder(folder, sizeof folder);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(path, size, "%s/Snapshot %s.state", folder, stamp);
}

static void state_path(char *path, size_t size, machine_t *machine, const char *rom_path) {
    char base[1024];
    data_folder(base, sizeof base);
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

typedef struct {
    uint32_t memory;
    screen_size_t screen;
    uint32_t speed;
    uint32_t host_time;
    uint32_t scale;
    uint32_t connect_at_launch;
    uint32_t system;
    char     machine[64];
    char     serial_device[1024];
    uint32_t display;
    char     user_agent[256];
    char     shared_folder[1024];
    uint32_t full_brightness;
} settings_t;

static void settings_path(char *path, size_t size) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char base[1024];
    if (config_home && config_home[0] == '/') snprintf(base, sizeof base, "%s/velo-emu", config_home);
#ifdef __APPLE__
    else data_folder(base, sizeof base);
#else
    else snprintf(base, sizeof base, "%s/.config/velo-emu", getenv("HOME") ? getenv("HOME") : ".");
#endif
    SDL_CreateDirectory(base);
    snprintf(path, size, "%s/emu.ini", base);
}

static const uint32_t SCALES[] = { 50, 75, 100, 150, 200 };
#define SCALE_COUNT (int)(sizeof SCALES / sizeof SCALES[0])

static int scale_index(uint32_t scale) {
    for (int i = 0; i < SCALE_COUNT; i++) {
        if (SCALES[i] == scale) return i;
    }
    return -1;
}

static void copy_setting(char *destination, size_t size, const char *value) {
    size_t length = strcspn(value, "\r\n");
    if (length >= size) length = size - 1;
    memcpy(destination, value, length);
    destination[length] = 0;
}

static settings_t settings_load(void) {
    settings_t settings = { .memory = 4, .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .speed = 1, .host_time = 1, .scale = 100, .display = VIEW_SIMULATED, .user_agent = NET_GATEWAY_DEFAULT_USER_AGENT, .full_brightness = 1 };
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "r");
    if (!file) return settings;
    char line[1200];
    unsigned value;
    while (fgets(line, sizeof line, file)) {
        if (sscanf(line, "memory=%u", &value) == 1) settings.memory = value;
        else if (!strncmp(line, "screen=", 7)) {
            char size[32];
            copy_setting(size, sizeof size, line + 7);
            screen_parse(size, &settings.screen);
        }
        else if (sscanf(line, "speed=%u", &value) == 1) settings.speed = value;
        else if (sscanf(line, "host_time=%u", &value) == 1) settings.host_time = value;
        else if (sscanf(line, "scale=%u", &value) == 1 && scale_index(value) >= 0) settings.scale = value;
        else if (sscanf(line, "connect_at_launch=%u", &value) == 1) settings.connect_at_launch = value;
        else if (sscanf(line, "system=%u", &value) == 1) settings.system = value;
        else if (!strncmp(line, "machine=", 8)) copy_setting(settings.machine, sizeof settings.machine, line + 8);
        else if (sscanf(line, "display=%u", &value) == 1 && value <= VIEW_SHARP) settings.display = value;
        else if (!strncmp(line, "user_agent=", 11)) copy_setting(settings.user_agent, sizeof settings.user_agent, line + 11);
        else if (!strncmp(line, "serial_device=", 14)) copy_setting(settings.serial_device, sizeof settings.serial_device, line + 14);
        else if (!strncmp(line, "shared_folder=", 14)) copy_setting(settings.shared_folder, sizeof settings.shared_folder, line + 14);
        else if (sscanf(line, "full_brightness=%u", &value) == 1) settings.full_brightness = value != 0;
    }
    fclose(file);
    return settings;
}

static void settings_save(const settings_t *settings) {
    char path[1100];
    settings_path(path, sizeof path);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "memory=%u\nscreen=%ux%u\nspeed=%u\nhost_time=%u\nscale=%u\ndisplay=%u\nconnect_at_launch=%u\nsystem=%u\nmachine=%s\nserial_device=%s\nuser_agent=%s\nshared_folder=%s\nfull_brightness=%u\n", settings->memory,
            settings->screen.width, settings->screen.height, settings->speed, settings->host_time, settings->scale, settings->display, settings->connect_at_launch, settings->system, settings->machine, settings->serial_device,
            settings->user_agent, settings->shared_folder, settings->full_brightness);
    fclose(file);
}

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

static void set_title(SDL_Window *window, const char *name, const char *notice, bool paused, bool suspended) {
    char base[200], title[1400];
    snprintf(base, sizeof base, "%s (%s)", WINDOW_TITLE, name);
    if (notice) snprintf(title, sizeof title, "%s: %s", base, notice);
    else if (paused) snprintf(title, sizeof title, "%s, paused", base);
    else if (suspended) snprintf(title, sizeof title, "%s, suspended", base);
    else snprintf(title, sizeof title, "%s", base);
    if (strcmp(SDL_GetWindowTitle(window), title)) SDL_SetWindowTitle(window, title);
}

typedef struct {
    char   path[3][1024];
    size_t size[3];
    int    rank[3];
} rom_set_t;

enum { ROM_UNMARKED, ROM_PATCHED, ROM_PATCHED_115K };

static void rom_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/roms", base);
    SDL_CreateDirectory(path);
}

#define ROM_MIN_BYTES   (1024 * 1024)
#define CARD_MIN_BYTES  (1024 * 1024)
#define ROM_MAX_BYTES   (64 * 1024 * 1024)
#define ROM_PROBE_CACHE 32

typedef struct {
    char     path[1024];
    off_t    size;
    time_t   modified;
    int      system;
    uint32_t screens;
    int      rank;
} rom_probe_t;

static rom_probe_t rom_probes[ROM_PROBE_CACHE];
static int rom_probe_count = 0;
static int rom_probe_next = 0;

static int rom_system(const char *path, uint32_t *screens, int *rank) {
    *screens = 0;
    *rank = ROM_UNMARKED;
    size_t size;
    uint8_t *rom = file_read(path, &size);
    if (!rom) return 0;
    char sets[512];
    if (marker_patch_sets(rom, size, sets, sizeof sets)) *rank = marker_has_set(sets, "pc-link-115k") ? ROM_PATCHED_115K : ROM_PATCHED;
    char error[256];
    machine_t *machine = machine_create(rom, size, error, sizeof error);
    free(rom);
    if (!machine) return 0;
    int system = machine_rom_system(machine);
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
    probe->system = rom_system(path, &probe->screens, &probe->rank);
    *screens = probe->screens;
    *rank = probe->rank;
    return probe->system;
}

static void find_roms(rom_set_t *roms) {
    memset(roms, 0, sizeof *roms);
    char folder[1100];
    rom_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[sizeof roms->path[0]];
        if (snprintf(path, sizeof path, "%s/%s", folder, entry->d_name) >= (int)sizeof path) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < ROM_MIN_BYTES || info.st_size > ROM_MAX_BYTES) continue;
        uint32_t screens;
        int rank;
        int system = cached_rom_system(path, &info, &screens, &rank);
        size_t size = (size_t)info.st_size;
        bool better = !roms->path[system][0] || rank > roms->rank[system] || (rank == roms->rank[system] && size > roms->size[system]);
        if (!system || !better) continue;
        memcpy(roms->path[system], path, sizeof path);
        roms->size[system] = size;
        roms->rank[system] = rank;
    }
    closedir(dir);
}

static void cards_folder(char *path, size_t size) {
    char base[1024];
    data_folder(base, sizeof base);
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
    int           count;
    char          uris[PICK_MAX][1024];
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
    static import_pick_t pick;
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
        uint32_t screens;
        int rank;
        if (stat(path, &info) == 0 && info.st_size >= ROM_MIN_BYTES && info.st_size <= ROM_MAX_BYTES && rom_system(path, &screens, &rank)) {
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
    data_folder(base, sizeof base);
    snprintf(path, size, "%s/machines", base);
    SDL_CreateDirectory(path);
}

static uint32_t probe_rom(const char *path, char *label, size_t label_size) {
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < ROM_MIN_BYTES || info.st_size > ROM_MAX_BYTES) return 0;
    uint32_t screens;
    int rank;
    int system = cached_rom_system(path, &info, &screens, &rank);
    if (!system) return 0;
    static const char *RANKS[] = { "", " (patched)", " (patched, 115200)" };
    snprintf(label, label_size, "%s: %s%s", system == 1 ? "CE 1.0" : "CE 2.0", file_leaf_name(path), RANKS[rank]);
    return screens;
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
    struct stat info;
    uint32_t screens;
    if (stat(profile->rom, &info) != 0) return 0;
    int rank;
    return cached_rom_system(profile->rom, &info, &screens, &rank);
}

static bool legacy_state_path(const char *rom_path, char *state, size_t size) {
    size_t rom_size;
    uint8_t *rom = file_read(rom_path, &rom_size);
    if (!rom) return false;
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    free(rom);
    if (!machine) return false;
    state_path(state, size, machine, rom_path);
    machine_destroy(machine);
    return true;
}

static void migrate_profiles(profiles_t *profiles, const rom_set_t *roms, const settings_t *settings, const char *folder) {
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

static machine_t *start_machine(const profile_t *profile, uint32_t speed, const char *state_file, bool fresh,
                                char *state, size_t state_size, const char **notice) {
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
    machine_set_log(machine, log_message);
    machine_set_memory(machine, profile->memory);
    machine_set_screen(machine, profile->screen);
    machine_set_speed(machine, speed);
    machine_set_host_clock(machine, profile->host_time);
    machine_set_debug_output(machine, print_debug_line, NULL);
    start_debug_log(rom_path);
    if (state_file) snprintf(state, state_size, "%s", state_file);
    else if (profile->state[0]) snprintf(state, state_size, "%s", profile->state);
    else state_path(state, state_size, machine, rom_path);
    if (fresh) {
        backup_file(state);
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
    if (!existing) insert_library_card(machine);
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
    if (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) SDL_SetWindowFullscreen(window, false);
    int width, height;
    window_size(view_display(view), scale, &width, &height);
    SDL_SetWindowSize(window, width, height);
}

typedef struct {
    settings_t   *settings;
    serial_mode_t serial_mode;
    const char   *card, *disk, *state_file, *machine;
    bool          fresh;
    int           gdb_port;
    const char   *gdb_process;
    const char   *agent_socket;
} launch_t;

enum {
    LAUNCH_HEADING_MACHINE, LAUNCH_MACHINE, LAUNCH_STATE, LAUNCH_FRESH, LAUNCH_CARD, LAUNCH_DISK, LAUNCH_MEMORY, LAUNCH_SCREEN, LAUNCH_SPEED,
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
    [LAUNCH_HEADING_CONNECTIONS] = { NULL, NULL, "Connections", 0 },
    [LAUNCH_SERIAL] = { "serial", "net|pty|off|PORT", "COM1 on the PPP network, a pseudo-terminal, nothing, or a host serial port such as /dev/cu.usbserial-1", 0 },
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
    case LAUNCH_SERIAL:
        if (!strcmp(value, "net")) launch->serial_mode = SERIAL_NETWORK;
        else if (!strcmp(value, "pty")) launch->serial_mode = SERIAL_PTY;
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

int main(int argc, char **argv) {
    const char *rom_path = NULL;
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
    migrate_old_folders();
    settings_t settings = settings_load();
    launch_t launch = { &settings, settings.connect_at_launch ? SERIAL_NETWORK : SERIAL_OFF, NULL, NULL, NULL, NULL, false, 0, NULL, NULL };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&LAUNCH_SPEC, argc, argv, launch_option, &launch, positional, 1, &positional_count);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    if (positional_count) rom_path = positional[0];
    serial_mode_t serial_mode = launch.serial_mode;
    const char *card = launch.card, *disk = launch.disk, *state_file = launch.state_file;
    bool fresh = launch.fresh;
    static rom_set_t roms;
    find_roms(&roms);
    char profiles_folder[1100];
    machines_folder(profiles_folder, sizeof profiles_folder);
    static profiles_t profiles;
    profiles_load(&profiles, profiles_folder);
    if (!profiles.count) migrate_profiles(&profiles, &roms, &settings, profiles_folder);
    static profile_t current;
    int current_index = -1;
    if (rom_path) {
        current = (profile_t){ .memory = settings.memory, .screen = settings.screen, .host_time = settings.host_time != 0 };
        snprintf(current.rom, sizeof current.rom, "%s", rom_path);
        snprintf(current.name, sizeof current.name, "%s", file_leaf_name(rom_path));
    } else {
        if (launch.machine) {
            current_index = profile_find(&profiles, launch.machine);
            if (current_index < 0) { fprintf(stderr, "no machine called %s\n", launch.machine); return 2; }
        } else {
            current_index = settings.machine[0] ? profile_find(&profiles, settings.machine) : -1;
            if (current_index < 0) current_index = profiles.count ? 0 : -1;
        }
#ifdef __ANDROID__
        while (current_index < 0 && first_run_import()) {
            find_roms(&roms);
            migrate_profiles(&profiles, &roms, &settings, profiles_folder);
            current_index = profiles.count ? 0 : -1;
        }
#endif
        if (current_index < 0) return no_roms_dialog() ? 0 : 1;
        current = profiles.entries[current_index];
    }
    char state[1100];
    const char *startup_notice = NULL;
    machine_t *machine = start_machine(&current, settings.speed, state_file, fresh, state, sizeof state, &startup_notice);
    if (!machine) { fprintf(stderr, "%s\n", startup_notice); return 1; }
    if (current_index >= 0) {
        snprintf(settings.machine, sizeof settings.machine, "%s", current.id);
        settings_save(&settings);
    }
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
    static int16_t samples[AUDIO_CHUNK];

    bool running = true, pen_down = false, paused = false;
    bool held[256] = { false };
    uint64_t last = SDL_GetPerformanceCounter();
    double frequency = (double)SDL_GetPerformanceFrequency();
    double since_autosave = 0, since_backup = 0, notice_left = 0;
    uint64_t power_release_at = 0, backlight_release_at = 0;
    const char *notice = startup_notice;
    if (notice) notice_left = 6;
    static serial_t serial;
    serial = (serial_t){ SERIAL_OFF, NULL, -1, -1, "", settings.user_agent, settings.serial_device, 0, { 0 }, 0 };
    char rapi_socket[1024], sync_manifest[1024], desktop_notice[256], shared_notice[1200], paste_notice[64];
    static typer_t typer;
    static scroller_t scroller;
    static input_queue_t input;
    static char ports[SERIAL_PORT_MAX][64];
    int port_count = 0;
    double since_port_scan = 0;
    static dropped_t dropped;
    picked_t *picked = NULL;
    rapi_socket_path(rapi_socket, sizeof rapi_socket);
    rapi_data_path("sync-manifest.txt", sync_manifest, sizeof sync_manifest);
    desktop_t *desktop = desktop_create(rapi_socket, sync_manifest);
    uint64_t serial_reconnect_at = 0, serial_unplug_at = 0;
    serial_mode_t serial_reconnect_mode = SERIAL_OFF;
    serial_restored(&serial, machine, &serial_reconnect_at, &serial_reconnect_mode);
    if (serial_mode != SERIAL_OFF && serial_reconnect_at) {
        serial_reconnect_mode = serial_mode;
    } else if (serial_mode != SERIAL_OFF) {
        const char *result = serial_open(&serial, machine, serial_mode);
        if (!notice) { notice = result; notice_left = NOTICE_SECONDS * 2; }
    }

    if (launch.gdb_process && !launch.gdb_port) {
        fprintf(stderr, "velo: --gdb-process needs --gdb\n");
        return 2;
    }
    if (launch.gdb_port) {
        debugger = gdb_create(machine, launch.gdb_port, log_gdb);
        if (!debugger) {
            fprintf(stderr, "velo: cannot listen for GDB on port %d\n", launch.gdb_port);
            return 1;
        }
        if (launch.gdb_process) gdb_set_process(debugger, launch.gdb_process);
    }
    if (launch.agent_socket && !(agent = agent_create(launch.agent_socket, log_gdb))) {
        fprintf(stderr, "velo: cannot listen on agent socket %s\n", launch.agent_socket);
        return 1;
    }
    static runner_t runner;
    runner = (runner_t){ SDL_CreateMutex(), NULL, machine, &input, false, false, true, { 0 } };
    runner.thread = SDL_CreateThread(run_machine, "velo-machine", &runner);
    while (running) {
        SDL_Event event;
        uint64_t frame_start = SDL_GetTicksNS();
        bool events_seen = false;
        SDL_SetAtomicInt(&runner.waiting, 1);
        SDL_LockMutex(runner.lock);
        SDL_SetAtomicInt(&runner.waiting, 0);
        while (SDL_PollEvent(&event)) {
            events_seen = true;
            if (menu_event(&event)) {
                if (menu_active()) {
                    release_keys(&input, machine, held, -1);
                    if (pen_down) input_add(&input, machine, INPUT_PEN, false, 0, 0, 0);
                    pen_down = false;
                }
                continue;
            }
            switch (event.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_TERMINATING:
                running = false;
                break;
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                machine_save(machine, state, (int64_t)time(NULL));
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                bool down = event.type == SDL_EVENT_KEY_DOWN;
                uint8_t scancode;
                if (!find_scancode(key_layout, event.key.key, &scancode)) break;
                if (down) {
                    if (event.key.repeat || (event.key.mod & SDL_KMOD_GUI) || held[scancode]) break;
                    held[scancode] = true;
                    input_add(&input, machine, INPUT_KEY, true, 0, 0, scancode);
                } else if (held[scancode]) {
                    held[scancode] = false;
                    input_add(&input, machine, INPUT_KEY, false, 0, 0, scancode);
                }
                break;
            }
            case SDL_EVENT_MOUSE_WHEEL:
                scroller_add(&scroller, event.wheel.y, event.wheel.x);
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data && dropped.count < PICK_MAX) snprintf(dropped.paths[dropped.count++], sizeof dropped.paths[0], "%s", event.drop.data);
                break;
            case SDL_EVENT_DROP_COMPLETE:
                if (dropped.count) {
                    bool online = serial.gateway && net_gateway_online(serial.gateway);
                    notice = handle_drop(&dropped, machine, desktop, online && !desktop_busy(desktop));
                    notice_left = NOTICE_SECONDS * 2;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                find_roms(&roms);
#ifdef __ANDROID__
                SDL_SetWindowFullscreen(window, false);
                SDL_SetWindowFullscreen(window, true);
#endif
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                release_keys(&input, machine, held, -1);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    int x, y;
                    if (view_screen_position(view, event.button.x, event.button.y, &x, &y)) {
                        pen_down = true;
                        input_add(&input, machine, INPUT_PEN, true, x, y, 0);
                    }
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (pen_down) {
                    int x, y;
                    view_screen_position(view, event.motion.x, event.motion.y, &x, &y);
                    pen_move(&input, machine, x, y);
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.button == SDL_BUTTON_LEFT && pen_down) {
                    int x, y;
                    view_screen_position(view, event.button.x, event.button.y, &x, &y);
                    pen_down = false;
                    input_add(&input, machine, INPUT_PEN, false, x, y, 0);
                }
                break;
            default:
                if (pick_event_type && event.type == pick_event_type) {
                    free(picked);
                    picked = event.user.data1;
#ifdef __ANDROID__
                    localize_picked(picked);
#endif
                }
                break;
            }
        }

        release_keys(&input, machine, held, menu_modifiers());
        int switch_to = -1;
        for (int item = menu_poll(); item >= 0; item = menu_poll()) {
            release_keys(&input, machine, held, -1);
            switch (item) {
            case MENU_POWER:
                machine_power_button(machine, true);
                power_release_at = machine_cycles(machine) + (uint64_t)(POWER_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_PAUSE: paused = !paused; break;
            case MENU_SOFT_RESET: machine_soft_reset(machine); break;
            case MENU_NEW_MACHINE: {
                static dialog_rom_t rom_list[32];
                int rom_count = list_roms(rom_list, 32);
                dialog_machine_t chosen = { .memory = profile_system(&current) == 2 ? CE2_DEFAULT_MEMORY : 4, .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .host_time = settings.host_time != 0 };
                if (rom_count) snprintf(chosen.rom, sizeof chosen.rom, "%s", current.rom);
                events_seen = true;
                if (!dialog_new_machine(window, rom_list, rom_count, probe_rom, &chosen)) break;
                profile_t made = { .screen = chosen.screen, .memory = chosen.memory, .host_time = chosen.host_time };
                snprintf(made.rom, sizeof made.rom, "%s", chosen.rom);
                if (chosen.name[0]) snprintf(made.name, sizeof made.name, "%s", chosen.name);
                else profile_default_name(&made, profile_system(&made), made.name, sizeof made.name);
                profile_make_unique(&profiles, &made, profiles_folder);
                if (!profile_save(&made, profiles_folder)) {
                    notice = "could not save the new machine";
                    notice_left = NOTICE_SECONDS * 2;
                    break;
                }
                profiles_load(&profiles, profiles_folder);
                if (current.id[0]) current_index = profile_find(&profiles, current.id);
                switch_to = profile_find(&profiles, made.id);
                break;
            }
            case MENU_MANAGE_MACHINES: {
                const char *names[PROFILES_MAX];
                for (int i = 0; i < profiles.count; i++) names[i] = profiles.entries[i].name;
                int chosen = current_index >= 0 ? current_index : 0;
                events_seen = true;
                dialog_manage_t action = profiles.count ? dialog_manage_machines(window, names, profiles.count, current_index, &chosen) : DIALOG_MANAGE_CLOSE;
                if (action == DIALOG_MANAGE_CLOSE || chosen < 0 || chosen >= profiles.count) break;
                profile_t picked_profile = profiles.entries[chosen];
                static char manage_notice[300];
                if (action == DIALOG_MANAGE_RESET) {
                    if (!confirm_reset(window, picked_profile.name)) break;
                    if (chosen == current_index) {
                        backup_machine(machine, state);
                        since_backup = 0;
                        machine_reset(machine);
                    } else {
                        backup_file(picked_profile.state);
                        remove(picked_profile.state);
                    }
                    snprintf(manage_notice, sizeof manage_notice, "reset %s; the machine before it is in Snapshots/Backups", picked_profile.name);
                } else {
                    if (chosen == current_index) {
                        notice = "switch to another machine before deleting this one";
                        notice_left = NOTICE_SECONDS * 3;
                        break;
                    }
                    char title[160];
                    snprintf(title, sizeof title, "Delete %s?", picked_profile.name);
                    if (!confirm_action(window, title, "This removes the machine and its saved state. A backup of the state goes in Snapshots/Backups first.", "Delete")) break;
                    backup_file(picked_profile.state);
                    profile_delete(&picked_profile, profiles_folder);
                    char current_id[sizeof current.id];
                    snprintf(current_id, sizeof current_id, "%s", current.id);
                    profiles_load(&profiles, profiles_folder);
                    current_index = current_id[0] ? profile_find(&profiles, current_id) : -1;
                    snprintf(manage_notice, sizeof manage_notice, "deleted %s", picked_profile.name);
                }
                notice = manage_notice;
                notice_left = NOTICE_SECONDS * 3;
                break;
            }
            case MENU_SCALE_50:
            case MENU_SCALE_75:
            case MENU_SCALE_100:
            case MENU_SCALE_150:
            case MENU_SCALE_200:
            case MENU_ZOOM_IN:
            case MENU_ZOOM_OUT: {
                int index = scale_index(settings.scale);
                if (item == MENU_ZOOM_IN) index = index + 1 < SCALE_COUNT ? index + 1 : index;
                else if (item == MENU_ZOOM_OUT) index = index > 0 ? index - 1 : index;
                else index = item - MENU_SCALE_50;
                settings.scale = SCALES[index];
                settings_save(&settings);
                fit_window(window, view, settings.scale);
                break;
            }
            case MENU_FULL_SCREEN:
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
                break;
            case MENU_DISPLAY_SIMULATED:
            case MENU_DISPLAY_SHARP:
                settings.display = item == MENU_DISPLAY_SHARP ? VIEW_SHARP : VIEW_SIMULATED;
                settings_save(&settings);
                view_set_display(view, (view_display_t)settings.display);
                fit_window(window, view, settings.scale);
                break;
            case MENU_COPY_SCREEN:
                notice = copy_screen(view) ? "screen copied" : "could not copy the screen";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_SAVE_SCREENSHOT: {
                static char screenshot_notice[1200];
                char path[1100];
                if (save_screenshot(view, path, sizeof path)) snprintf(screenshot_notice, sizeof screenshot_notice, "saved %s", file_leaf_name(path));
                else snprintf(screenshot_notice, sizeof screenshot_notice, "could not save the screenshot");
                notice = screenshot_notice;
                notice_left = NOTICE_SECONDS * 2;
                break;
            }
            case MENU_CONNECT_AT_LAUNCH:
                settings.connect_at_launch = !settings.connect_at_launch;
                settings_save(&settings);
                break;
            case MENU_PASTE: {
                char *clipboard = SDL_GetClipboardText();
                size_t typed = clipboard ? typer_start(&typer, key_layout, clipboard) : 0;
                SDL_free(clipboard);
                snprintf(paste_notice, sizeof paste_notice, typed ? "typing %zu characters" : "nothing to type", typed);
                notice = paste_notice;
                notice_left = NOTICE_SECONDS;
                break;
            }
            case MENU_SAVE_STATE:
                notice = machine_save(machine, state, (int64_t)time(NULL)) ? "state saved" : "could not save state";
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_LOAD_STATE:
                backup_machine(machine, state);
                if (machine_load(machine, state, NULL)) {
                    serial_restored(&serial, machine, &serial_reconnect_at, &serial_reconnect_mode);
                    notice = "state loaded";
                } else {
                    notice = "no saved state";
                }
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_BACKLIGHT:
                machine_backlight_button(machine, true);
                backlight_release_at = machine_cycles(machine) + (uint64_t)(BACKLIGHT_PRESS_SECONDS * MACHINE_CLOCK_HZ);
                break;
            case MENU_SOUND: sound = !sound; break;
            case MENU_SHOW_STATE: reveal_file(state); break;
            case MENU_SAVE_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state" } };
                static char default_snapshot[1200];
                snapshot_default_name(default_snapshot, sizeof default_snapshot);
                SDL_ShowSaveFileDialog(pick_done, (void *)(intptr_t)PICK_SAVE_SNAPSHOT, window, filters, 1, default_snapshot);
                break;
            }
            case MENU_LOAD_SNAPSHOT: {
                static const SDL_DialogFileFilter filters[] = { { "Velo snapshot", "state;bin" } };
                static char folder[1100];
                snapshot_folder(folder, sizeof folder);
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
                static char import_notice[160];
                int cards, imported = import_files(&cards);
                find_roms(&roms);
                snprintf(import_notice, sizeof import_notice, "imported %d ROMs and %d cards", imported, cards);
                notice = import_notice;
                notice_left = NOTICE_SECONDS * 2;
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
                notice = "disk ejected";
                notice_left = NOTICE_SECONDS;
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
                    notice = serial_open(&serial, machine, SERIAL_DEVICE);
                    notice_left = NOTICE_SECONDS * 3;
                }
                break;
            case MENU_SERIAL_NETWORK:
            case MENU_SERIAL_PTY:
            case MENU_SERIAL_OFF:
                serial_reconnect_at = 0;
                notice = serial_open(&serial, machine, item == MENU_SERIAL_NETWORK ? SERIAL_NETWORK : item == MENU_SERIAL_PTY ? SERIAL_PTY : SERIAL_OFF);
                notice_left = NOTICE_SECONDS * 3;
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
                    notice = "allow All files access for Velo, then try again";
                    notice_left = NOTICE_SECONDS * 3;
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
                notice = shared_notice;
                notice_left = NOTICE_SECONDS;
                break;
            case MENU_EJECT_CARD:
                machine_eject_card(machine);
                notice = "card ejected";
                notice_left = NOTICE_SECONDS;
                break;
            }
        }
        if (switch_to >= 0 && switch_to < profiles.count && switch_to != current_index) {
            if (desktop_busy(desktop)) {
                notice = "busy with a desktop transfer";
                notice_left = NOTICE_SECONDS;
            } else {
                const char *switch_notice = NULL;
                char next_state[sizeof state];
                profile_t next_profile = profiles.entries[switch_to];
                machine_t *next = start_machine(&next_profile, settings.speed, NULL, false, next_state, sizeof next_state, &switch_notice);
                if (!next) {
                    notice = switch_notice;
                    notice_left = NOTICE_SECONDS * 2;
                } else {
                    serial_mode_t mode = serial.mode;
                    serial_close(&serial, machine);
                    if (pen_down) machine_touch(machine, false, 0, 0);
                    input_clear(&input);
                    pen_down = false;
                    typer.length = typer.position = 0;
                    machine_save(machine, state, (int64_t)time(NULL));
                    machine_destroy(machine);
                    machine = next;
                    memcpy(state, next_state, sizeof state);
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
                    runner.machine = machine;
                    runner.restart = true;
                    if (debugger) gdb_set_machine(debugger, machine);
                    static char switched_notice[160];
                    snprintf(switched_notice, sizeof switched_notice, "switched to %s", current.name);
                    notice = switch_notice ? switch_notice : switched_notice;
                    notice_left = NOTICE_SECONDS * 2;
                }
            }
        }
        bool velo_online = serial.gateway && net_gateway_online(serial.gateway);
        if (picked) {
            if (picked->kind == PICK_CARD) {
                notice = machine_insert_card(machine, picked->paths[0]) ? "card inserted" : "could not open card image";
                notice_left = NOTICE_SECONDS;
            } else if (picked->kind == PICK_DISK) {
                notice = machine_insert_disk(machine, picked->paths[0], false) ? "disk inserted" : "could not open disk image";
                notice_left = NOTICE_SECONDS;
            } else if (picked->kind == PICK_NEW_DISK) {
                static char disk_notice[1200];
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked->paths[0], has_extension(picked->paths[0], ".img") ? "" : ".img");
                bool made = create_blank_disk(path) && machine_insert_disk(machine, path, false);
                snprintf(disk_notice, sizeof disk_notice, made ? "inserted new disk %s; the Velo offers to format it" : "could not create %s", file_leaf_name(path));
                notice = disk_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked->kind == PICK_SEND) {
                const char *files[PICK_MAX + 1];
                for (int i = 0; i < picked->count; i++) files[i] = picked->paths[i];
                files[picked->count] = NULL;
                desktop_send(desktop, files);
            } else if (picked->kind == PICK_SAVE_SNAPSHOT) {
                static char snapshot_notice[1200];
                char path[1100];
                snprintf(path, sizeof path, "%s%s", picked->paths[0], has_extension(picked->paths[0], ".state") ? "" : ".state");
                bool saved = machine_save(machine, path, (int64_t)time(NULL));
#ifdef __ANDROID__
                if (saved && picked->export_uri[0]) {
                    saved = android_export(path, picked->export_uri);
                    remove(path);
                }
#endif
                snprintf(snapshot_notice, sizeof snapshot_notice, saved ? "saved snapshot %s" : "could not save %s", file_leaf_name(path));
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked->kind == PICK_LOAD_SNAPSHOT) {
                static char snapshot_notice[1200];
                if (machine_state_matches(machine, picked->paths[0])) backup_machine(machine, state);
                if (machine_load(machine, picked->paths[0], NULL)) {
                    serial_restored(&serial, machine, &serial_reconnect_at, &serial_reconnect_mode);
                    snprintf(snapshot_notice, sizeof snapshot_notice, "loaded snapshot %s", file_leaf_name(picked->paths[0]));
                } else {
                    snprintf(snapshot_notice, sizeof snapshot_notice, "%s isn't a snapshot of this ROM", file_leaf_name(picked->paths[0]));
                }
                notice = snapshot_notice;
                notice_left = NOTICE_SECONDS * 2;
            } else if (picked->kind == PICK_FETCH) {
                desktop_fetch(desktop, picked->paths[0]);
            } else if (picked->kind == PICK_SHARED) {
                snprintf(settings.shared_folder, sizeof settings.shared_folder, "%s", picked->paths[0]);
                settings_save(&settings);
                snprintf(shared_notice, sizeof shared_notice, "sharing %s with \\My Documents", file_leaf_name(settings.shared_folder));
                notice = shared_notice;
                notice_left = NOTICE_SECONDS * 2;
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
                serial_open(&serial, machine, SERIAL_OFF);
                serial_reconnect_mode = SERIAL_NETWORK;
                serial_reconnect_at = machine_cycles(machine) + 2ull * MACHINE_CLOCK_HZ;
            }
        }
        if (desktop_take_status(desktop, desktop_notice, sizeof desktop_notice)) {
            notice = desktop_notice;
            notice_left = NOTICE_SECONDS * 2;
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
        reap_reveal_children();
        if (since_port_scan <= 0) {
            since_port_scan = PORT_SCAN_SECONDS;
            port_count = list_serial_ports(ports, SERIAL_PORT_MAX);
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
        for (int scale_item = MENU_SCALE_50; scale_item <= MENU_SCALE_200; scale_item++) menu_set_checked(scale_item, settings.scale == SCALES[scale_item - MENU_SCALE_50]);
        menu_set_enabled(MENU_ZOOM_IN, settings.scale < SCALES[SCALE_COUNT - 1]);
        menu_set_enabled(MENU_ZOOM_OUT, settings.scale > SCALES[0]);
        menu_set_checked(MENU_FULL_SCREEN, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0);
        menu_set_checked(MENU_DISPLAY_SIMULATED, settings.display == VIEW_SIMULATED);
        menu_set_checked(MENU_DISPLAY_SHARP, settings.display == VIEW_SHARP);
        menu_set_checked(MENU_SPEED_1, machine_speed(machine) == 1);
        menu_set_checked(MENU_SPEED_2, machine_speed(machine) == 2);
        menu_set_checked(MENU_SPEED_4, machine_speed(machine) == 4);
        menu_set_checked(MENU_SPEED_8, machine_speed(machine) == 8);

        uint64_t now = SDL_GetPerformanceCounter();
        double elapsed = (double)(now - last) / frequency;
        last = now;
        if (elapsed > MAX_FRAME_SLICE) elapsed = MAX_FRAME_SLICE;
        if (notice_left > 0) {
            notice_left -= elapsed;
            if (notice_left <= 0) notice = NULL;
        }
        set_title(window, current.name, notice, paused, machine_suspended(machine));
        since_autosave += elapsed;
        if (!paused) since_backup += elapsed;
        since_port_scan -= elapsed;
        if (since_backup >= BACKUP_SECONDS) {
            since_backup = 0;
            backup_machine(machine, state);
        }
        if (since_autosave >= AUTOSAVE_SECONDS) {
            since_autosave = 0;
            machine_save(machine, state, (int64_t)time(NULL));
        }
        runner.paused = paused;
        typer_step(&typer, machine);
        scroller_step(&scroller, machine);
        serial_pump(&serial, machine);
        if (serial_reconnect_at && machine_cycles(machine) >= serial_reconnect_at) {
            serial_reconnect_at = 0;
            notice = serial_open(&serial, machine, serial_reconnect_mode);
            notice_left = NOTICE_SECONDS * 2;
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
        SDL_UnlockMutex(runner.lock);
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

    SDL_SetAtomicInt(&runner.waiting, 1);
    SDL_LockMutex(runner.lock);
    runner.stop = true;
    SDL_SetAtomicInt(&runner.waiting, 0);
    SDL_UnlockMutex(runner.lock);
    SDL_WaitThread(runner.thread, NULL);
    gdb_destroy(debugger);
    debugger = NULL;
    agent_destroy(agent);
    agent = NULL;
    SDL_DestroyMutex(runner.lock);
    free(picked);
    machine_save(machine, state, (int64_t)time(NULL));
    serial_close(&serial, machine);
    desktop_destroy(desktop);
    if (verbose) machine_dump_state(machine);
    SDL_DestroyAudioStream(audio);
    view_destroy(view);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    machine_destroy(machine);
    return 0;
}

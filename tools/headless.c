#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "core/agent.h"
#include "core/gdb.h"
#include "core/key_text.h"
#include "core/lcd.h"
#include "core/machine.h"
#include "net/net_gateway.h"
#include "net/serial_link.h"
#include "util/file.h"
#include "util/options.h"
#include "util/png.h"

static void log_stderr(const char *message) {
    fputs(message, stderr);
}

static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static gdb_t *debugger;
static agent_t *agent;

#define AGENT_POLL_CYCLES (MACHINE_CLOCK_HZ / 100)

static void run_cycles(machine_t *machine, uint64_t cycles) {
    if (!debugger) machine_run(machine, cycles);
    else if (!gdb_run(debugger, cycles)) stop_requested = 1;
}

static void advance(machine_t *machine, uint64_t cycles) {
    if (!agent) {
        run_cycles(machine, cycles);
        return;
    }
    while (cycles && !stop_requested) {
        uint64_t step = cycles < AGENT_POLL_CYCLES ? cycles : AGENT_POLL_CYCLES;
        run_cycles(machine, step);
        agent_poll(agent, machine_mailbox(machine));
        cycles -= step;
    }
}

static double wall_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void pace(machine_t *machine, double realtime, double wall_start, uint64_t cycles_start) {
    if (realtime <= 0) return;
    double due = wall_start + (double)(machine_cycles(machine) - cycles_start) / MACHINE_CLOCK_HZ / realtime;
    double ahead = due - wall_seconds();
    if (ahead <= 0) return;
    struct timespec wait = { (time_t)ahead, (long)((ahead - (double)(time_t)ahead) * 1e9) };
    nanosleep(&wait, NULL);
}

static void type_text(machine_t *machine, const char *text) {
    for (const char *c = text; *c; c++) {
        char ch = *c;
        if (ch == '\\' && c[1] == 'n') { ch = '\n'; c++; }
        uint8_t scancode;
        bool shifted;
        if (!key_text_find(machine_key_layout(machine), ch, &scancode, &shifted)) continue;
        if (shifted) machine_key(machine, KEY_TEXT_SHIFT, false);
        machine_key(machine, scancode, false);
        advance(machine, MACHINE_CLOCK_HZ / 50);
        machine_key(machine, scancode, true);
        if (shifted) machine_key(machine, KEY_TEXT_SHIFT, true);
        advance(machine, MACHINE_CLOCK_HZ / 50);
    }
}

static bool write_panel_png(const char *path, machine_t *machine, int cell, int backlight) {
    screen_size_t size = machine_screen_size(machine);
    lcd_set_size(size.width, size.height);
    lcd_compose_setup(cell);
    lcd_set_power(machine_lcd_enabled(machine));
    lcd_set_backlight(backlight < 0 ? machine_backlight(machine) : backlight != 0);
    machine_screen(machine, lcd_framebuffer);
    lcd_compose(10.0f);
    uint8_t *png;
    size_t length;
    if (!png_encode(lcd_compose_pixels(), lcd_compose_width(), lcd_compose_height(), &png, &length)) return false;
    FILE *file = fopen(path, "wb");
    bool written = file && fwrite(png, 1, length, file) == length;
    if (file) fclose(file);
    free(png);
    return written;
}

static void write_pgm(const char *path, const uint8_t *levels, screen_size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P5\n%d %d\n255\n", size.width, size.height);
    for (int i = 0; i < size.width * size.height; i++) fputc(255 - levels[i] * 17, file);
    fclose(file);
}

typedef struct {
    const char *rom_path;
    double seconds;
    const char *png, *pgm, *load, *save, *wav, *card, *disk;
    int png_cell, png_backlight;
    bool trace_pc, host_time, optimisations, verify_optimisations;
    double key_times[32];
    unsigned key_codes[32][4];
    int key_lengths[32];
    int key_count;
    double tap_times[32];
    int tap_x[32], tap_y[32];
    double tap_hold[32];
    int tap_count;
    double power_times[8];
    int power_count;
    double soft_reset_at;
    double disk_insert_times[4];
    const char *disk_insert_images[4];
    int disk_insert_count;
    double disk_eject_times[4];
    int disk_eject_count;
    double backlight_times[8];
    int backlight_count;
    double cable_at, net_at, replug_at, realtime;
    int tcp_port;
    uint32_t watches[MACHINE_WATCH_MAX];
    int watch_count;
    net_gateway_options_t net_options;
    double send_times[8];
    const char *send_text[8];
    int send_count;
    double type_times[16];
    const char *type_strings[16];
    int type_count;
    uint32_t memory, speed;
    screen_size_t screen;
    bool debug_output;
    bool seconds_given;
    int gdb_port;
    const char *gdb_process;
    const char *agent_socket;
} run_t;

enum {
    OPT_HEADING_RUN, OPT_SECONDS, OPT_LOAD, OPT_SAVE, OPT_CARD, OPT_DISK, OPT_MEMORY, OPT_SCREEN, OPT_SPEED, OPT_REALTIME, OPT_HOST_TIME, OPT_OPTIMISATIONS, OPT_VERIFY_OPTIMISATIONS,
    OPT_HEADING_INPUT, OPT_TAP, OPT_KEY, OPT_TYPE, OPT_POWER, OPT_BACKLIGHT, OPT_SOFT_RESET, OPT_INSERT_DISK, OPT_EJECT_DISK,
    OPT_HEADING_NET, OPT_NET, OPT_RAPI, OPT_RAPI_PORT, OPT_TCP, OPT_AGENT, OPT_USER_AGENT, OPT_REPLUG, OPT_CABLE, OPT_CABLE_SEND,
    OPT_HEADING_OUTPUT, OPT_PGM, OPT_PNG, OPT_PNG_CELL, OPT_PNG_BACKLIGHT, OPT_WAV, OPT_TRACE_PC, OPT_WATCH_PC, OPT_DEBUG_OUTPUT, OPT_GDB,
    OPT_GDB_PROCESS,
};

static const option_t OPTIONS[] = {
    [OPT_HEADING_RUN] = { NULL, NULL, "Running", 0 },
    [OPT_SECONDS] = { "seconds", "N", "emulated seconds to run (default 5)", 0 },
    [OPT_LOAD] = { "load", "STATE", "start from a saved state", 0 },
    [OPT_SAVE] = { "save", "STATE", "save the machine at the end (and on SIGTERM)", 0 },
    [OPT_CARD] = { "card", "IMAGE", "insert a PC Card image, after --load", 0 },
    [OPT_DISK] = { "disk", "IMAGE", "attach a disk image to the paravirtual disk, after --load", 0 },
    [OPT_MEMORY] = { "memory", "MB", "RAM for a cold boot: 4, 8, 16, 20 or 32", 0 },
    [OPT_SCREEN] = { "screen", "WxH", "screen for a cold boot: 480x240, 640x240, 640x480 or 800x600, where the ROM supports it", 0 },
    [OPT_SPEED] = { "speed", "N", "CPU speed multiple: 1, 2, 4 or 8", 0 },
    [OPT_REALTIME] = { "realtime", "[N]", "pace emulated time at N times real time (default 1), for RAPI clients", 0 },
    [OPT_HOST_TIME] = { "host-time", NULL, "set the clock from this computer at a cold boot", 0 },
    [OPT_OPTIMISATIONS] = { "optimisations", NULL, "run CE's ROM compression natively and skip busy-waits on the clock", 0 },
    [OPT_VERIFY_OPTIMISATIONS] = { "verify-optimisations", NULL, "run CE's own code for each optimisation, and report where the native version would differ", 0 },
    [OPT_HEADING_INPUT] = { NULL, NULL, "Input, at emulated times in seconds", 0 },
    [OPT_TAP] = { "tap", "SECONDS:X:Y[:HOLD]", "hold the pen at a screen position, for 0.5 s by default (0.08 for double taps)", 32 },
    [OPT_KEY] = { "key", "SECONDS:SCANCODE[+SCANCODE]", "press Velo scancodes (hex) together for 50 ms; the backlight key is 5E", 32 },
    [OPT_TYPE] = { "type", "SECONDS:TEXT", "type text, with \\n for Enter", 16 },
    [OPT_POWER] = { "power", "SECONDS", "press the power button for 200 ms", 8 },
    [OPT_BACKLIGHT] = { "backlight", "SECONDS", "press the backlight button for 100 ms", 8 },
    [OPT_SOFT_RESET] = { "soft-reset", "SECONDS", "soft-reset the machine", 0 },
    [OPT_INSERT_DISK] = { "insert-disk", "SECONDS:IMAGE", "attach a disk image to the paravirtual disk, replacing any attached", 4 },
    [OPT_EJECT_DISK] = { "eject-disk", "SECONDS", "detach the paravirtual disk's image", 4 },
    [OPT_HEADING_NET] = { NULL, NULL, "Serial and network", 0 },
    [OPT_NET] = { "net", "SECONDS", "connect COM1 to the PPP gateway and web proxy", 0 },
    [OPT_AGENT] = { "agent", "SOCKET", "pass messages between a guest agent's break 0x51CE mailbox and one client on this Unix socket", 0 },
    [OPT_RAPI] = { "rapi", "SOCKET", "expose the Velo's RAPI port on a Unix socket, for velo-rapi --socket", 0 },
    [OPT_RAPI_PORT] = { "rapi-port", "PORT", "expose the Velo's RAPI port on this TCP port on all interfaces, for velo-rapi --connect", 0 },
    [OPT_TCP] = { "tcp", "PORT", "offer COM1 as raw bytes on this TCP port on all interfaces, with the cable connected while a client is attached", 0 },
    [OPT_USER_AGENT] = { "user-agent", "TEXT", "the web proxy's user agent", 0 },
    [OPT_REPLUG] = { "replug", "SECONDS", "unplug the --net cable and plug it back in 2 seconds later, as the app does after a speed change", 0 },
    [OPT_CABLE] = { "cable", "SECONDS", "connect a bare serial cable, with nothing at the other end", 0 },
    [OPT_CABLE_SEND] = { "cable-send", "SECONDS:TEXT", "send bytes down the cable (\\r and \\n allowed); anything CE sends is printed", 8 },
    [OPT_HEADING_OUTPUT] = { NULL, NULL, "Output and debugging", 0 },
    [OPT_PGM] = { "pgm", "FILE", "save the raw greyscale screen at the end", 0 },
    [OPT_PNG] = { "png", "FILE", "save the screen through the simulated LCD at the end", 0 },
    [OPT_PNG_CELL] = { "png-cell", "N", "device pixels per LCD pixel for --png (default 4)", 0 },
    [OPT_PNG_BACKLIGHT] = { "png-backlight", "on|off", "draw --png lit or unlit, whatever the backlight is doing", 0 },
    [OPT_WAV] = { "wav", "FILE", "write the sound output, with silences removed", 0 },
    [OPT_TRACE_PC] = { "trace-pc", NULL, "print the program counter every emulated second", 0 },
    [OPT_WATCH_PC] = { "watch-pc", "VA", "log calls reaching an address (slot-relative below 0x02000000)", MACHINE_WATCH_MAX },
    [OPT_DEBUG_OUTPUT] = { "debug-output", NULL, "print CE's debug output (OutputDebugString, kernel messages) to stderr", 0 },
    [OPT_GDB] = { "gdb", "PORT", "wait for GDB on 127.0.0.1:PORT before running; runs until GDB detaches or kills, unless --seconds is given", 0 },
    [OPT_GDB_PROCESS] = { "gdb-process", "NAME", "debug one process, e.g. maths.exe: breakpoints below 0x02000000 only stop there, and GDB stops when it starts", 0 },
};


static bool print_debug_output;

static void print_debug_line(void *context, const char *line) {
    (void)context;
    if (print_debug_output) fprintf(stderr, "debug: %s\n", line);
    if (debugger) gdb_debug_line(debugger, line);
}

static bool parse_option(void *context, int option, const char *value, char *error, size_t error_size) {
    run_t *run = context;
    double number;
    long integer;
    const char *rest;
    switch (option) {
    case OPT_SECONDS:
        run->seconds_given = true;
        return option_number(value, &run->seconds) && run->seconds > 0;
    case OPT_LOAD: run->load = value; return true;
    case OPT_SAVE: run->save = value; return true;
    case OPT_CARD: run->card = value; return true;
    case OPT_DISK: run->disk = value; return true;
    case OPT_MEMORY:
        if (!option_integer(value, 10, &integer) || (integer != 4 && integer != 8 && integer != 16 && integer != 20 && integer != 32)) return false;
        run->memory = (uint32_t)integer;
        return true;
    case OPT_SCREEN: return screen_parse(value, &run->screen);
    case OPT_SPEED:
        if (!option_integer(value, 10, &integer) || (integer != 1 && integer != 2 && integer != 4 && integer != 8)) return false;
        run->speed = (uint32_t)integer;
        return true;
    case OPT_REALTIME:
        run->realtime = 1;
        return !value || (option_number(value, &run->realtime) && run->realtime > 0);
    case OPT_HOST_TIME: run->host_time = true; return true;
    case OPT_OPTIMISATIONS: run->optimisations = true; return true;
    case OPT_VERIFY_OPTIMISATIONS: run->optimisations = run->verify_optimisations = true; return true;
    case OPT_TAP: {
        int n = run->tap_count;
        if (!option_timed(value, &run->tap_times[n], &rest)) return false;
        char tail;
        run->tap_hold[n] = 0.5;
        int got = sscanf(rest, "%d:%d:%lf%c", &run->tap_x[n], &run->tap_y[n], &run->tap_hold[n], &tail);
        if (got != 2 && got != 3) return false;
        if (run->tap_x[n] < 0 || run->tap_x[n] >= SCREEN_MAX_WIDTH || run->tap_y[n] < 0 || run->tap_y[n] >= SCREEN_MAX_HEIGHT) {
            snprintf(error, error_size, "--tap position %d,%d is off the screen", run->tap_x[n], run->tap_y[n]);
            return false;
        }
        run->tap_count++;
        return true;
    }
    case OPT_KEY: {
        int n = run->key_count;
        if (!option_timed(value, &run->key_times[n], &rest)) return false;
        run->key_lengths[n] = 0;
        while (*rest) {
            char *end;
            long code = strtol(rest, &end, 16);
            if (end == rest || code < 0 || code > 0xFF || run->key_lengths[n] == 4 || (*end && *end != '+')) return false;
            run->key_codes[n][run->key_lengths[n]++] = (unsigned)code;
            rest = *end ? end + 1 : end;
            if (*end && !*rest) return false;
        }
        if (!run->key_lengths[n]) return false;
        run->key_count++;
        return true;
    }
    case OPT_TYPE:
        if (!option_timed(value, &run->type_times[run->type_count], &rest)) return false;
        run->type_strings[run->type_count++] = rest;
        return true;
    case OPT_POWER:
        if (!option_number(value, &number) || number < 0) return false;
        run->power_times[run->power_count++] = number;
        return true;
    case OPT_BACKLIGHT:
        if (!option_number(value, &number) || number < 0) return false;
        run->backlight_times[run->backlight_count++] = number;
        return true;
    case OPT_SOFT_RESET: return option_number(value, &run->soft_reset_at) && run->soft_reset_at >= 0;
    case OPT_INSERT_DISK:
        if (!option_timed(value, &run->disk_insert_times[run->disk_insert_count], &rest)) return false;
        run->disk_insert_images[run->disk_insert_count++] = rest;
        return true;
    case OPT_EJECT_DISK:
        if (!option_number(value, &number) || number < 0) return false;
        run->disk_eject_times[run->disk_eject_count++] = number;
        return true;
    case OPT_NET: return option_number(value, &run->net_at) && run->net_at >= 0;
    case OPT_RAPI: run->net_options.rapi_socket = value; return true;
    case OPT_RAPI_PORT: {
        long port;
        if (!option_integer(value, 10, &port) || port < 1 || port > 65535) return false;
        run->net_options.rapi_port = (int)port;
        return true;
    }
    case OPT_TCP: {
        long port;
        if (!option_integer(value, 10, &port) || port < 1 || port > 65535) return false;
        run->tcp_port = (int)port;
        return true;
    }
    case OPT_AGENT: run->agent_socket = value; return true;
    case OPT_USER_AGENT: run->net_options.user_agent = value; return true;
    case OPT_REPLUG: return option_number(value, &run->replug_at) && run->replug_at >= 0;
    case OPT_CABLE: return option_number(value, &run->cable_at) && run->cable_at >= 0;
    case OPT_CABLE_SEND:
        if (!option_timed(value, &run->send_times[run->send_count], &rest)) return false;
        run->send_text[run->send_count++] = rest;
        return true;
    case OPT_PGM: run->pgm = value; return true;
    case OPT_PNG: run->png = value; return true;
    case OPT_PNG_CELL:
        if (!option_integer(value, 10, &integer) || integer < 2 || integer > 16) return false;
        run->png_cell = (int)integer;
        return true;
    case OPT_PNG_BACKLIGHT:
        if (strcmp(value, "on") && strcmp(value, "off")) return false;
        run->png_backlight = !strcmp(value, "on");
        return true;
    case OPT_WAV: run->wav = value; return true;
    case OPT_TRACE_PC: run->trace_pc = true; return true;
    case OPT_DEBUG_OUTPUT: run->debug_output = true; return true;
    case OPT_GDB:
        if (!option_integer(value, 10, &integer) || integer < 1 || integer > 65535) return false;
        run->gdb_port = (int)integer;
        return true;
    case OPT_GDB_PROCESS: run->gdb_process = value; return true;
    case OPT_WATCH_PC:
        if (!option_integer(value, 0, &integer) || integer < 0) return false;
        run->watches[run->watch_count++] = (uint32_t)integer;
        return true;
    }
    return false;
}

static const option_spec_t SPEC = {
    "headless", "ROM [OPTIONS]",
    "Runs the Velo without a window, for tests and scripts. ROM is a CE 1.0 nk.bin, a CE 2.0 card ROM or merged image, or a B000FF image.",
    OPTIONS, (int)(sizeof OPTIONS / sizeof OPTIONS[0]),
    "Events at or after --seconds don't happen, and are reported. Options taking a value also accept it as the next argument.",
};

int main(int argc, char **argv) {
    static run_t run;
    run = (run_t){ .seconds = 5, .png_cell = 4, .png_backlight = -1, .soft_reset_at = -1, .cable_at = -1, .net_at = -1, .replug_at = -1,
                   .net_options = { NET_GATEWAY_DEFAULT_USER_AGENT, NULL, 0 } };
    static serial_link_t serial;
    serial_link_init(&serial, log_stderr);
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&SPEC, argc, argv, parse_option, &run, positional, 1, &positional_count);
    if (parsed == OPTIONS_EXIT) return 0;
    if (parsed == OPTIONS_ERROR) return 2;
    if (!positional_count) {
        fprintf(stderr, "headless: no ROM given (see --help)\n");
        return 2;
    }
    run.rom_path = positional[0];
    if (run.gdb_port && !run.seconds_given) run.seconds = 1e7;
    double latest = run.soft_reset_at;
    for (int k = 0; k < run.key_count; k++) if (run.key_times[k] > latest) latest = run.key_times[k];
    for (int t = 0; t < run.tap_count; t++) if (run.tap_times[t] > latest) latest = run.tap_times[t];
    for (int b = 0; b < run.power_count; b++) if (run.power_times[b] > latest) latest = run.power_times[b];
    for (int b = 0; b < run.backlight_count; b++) if (run.backlight_times[b] > latest) latest = run.backlight_times[b];
    for (int k = 0; k < run.type_count; k++) if (run.type_times[k] > latest) latest = run.type_times[k];
    for (int k = 0; k < run.send_count; k++) if (run.send_times[k] > latest) latest = run.send_times[k];
    for (int k = 0; k < run.disk_insert_count; k++) if (run.disk_insert_times[k] > latest) latest = run.disk_insert_times[k];
    for (int k = 0; k < run.disk_eject_count; k++) if (run.disk_eject_times[k] > latest) latest = run.disk_eject_times[k];
    if (run.cable_at > latest) latest = run.cable_at;
    if (run.net_at > latest) latest = run.net_at;
    if (run.replug_at >= 0 && run.replug_at + 2 > latest) latest = run.replug_at + 2;
    if (latest >= run.seconds) fprintf(stderr, "headless: an event at %.2f s is at or after --seconds=%.2f and won't happen\n", latest, run.seconds);
    if (run.tcp_port) {
        if (run.net_at >= 0) { fprintf(stderr, "headless: --net and --tcp can't both be given\n"); return 2; }
        serial.tcp_port = run.tcp_port;
        const char *failure = serial_link_open(&serial, SERIAL_TCP, NULL);
        if (failure) { fprintf(stderr, "headless: %s\n", failure); return 1; }
    }
    if (run.agent_socket && !(agent = agent_create(run.agent_socket, log_stderr))) {
        fprintf(stderr, "cannot listen on agent socket %s\n", run.agent_socket);
        return 1;
    }
    size_t rom_size;
    uint8_t *rom = file_read(run.rom_path, &rom_size);
    if (!rom) { fprintf(stderr, "cannot read %s\n", run.rom_path); return 1; }
    char error[256];
    machine_t *machine = machine_create(rom, rom_size, error, sizeof error);
    if (!machine) { fprintf(stderr, "%s\n", error); return 1; }
    machine_set_log(machine, log_stderr);
    if (run.memory) machine_set_memory(machine, run.memory);
    if (run.screen.width && !machine_set_screen(machine, run.screen)) {
        fprintf(stderr, "this ROM can't run at %ux%u\n", run.screen.width, run.screen.height);
        return 1;
    }
    if (run.speed) machine_set_speed(machine, run.speed);
    machine_set_host_clock(machine, run.host_time);
    machine_set_optimisations(machine, run.optimisations);
    if (run.verify_optimisations && !machine_set_verify_optimisations(machine, true)) { fprintf(stderr, "cannot verify optimisations\n"); return 1; }
    print_debug_output = run.debug_output;
    if (run.debug_output || run.gdb_port) machine_set_debug_output(machine, print_debug_line, NULL);
    if (run.load && !machine_load(machine, run.load, NULL)) { fprintf(stderr, "cannot load state %s\n", run.load); return 1; }
    if (run.load) machine_serial_connect(machine, false);
    for (int w = 0; w < run.watch_count; w++) machine_watch_pc(machine, run.watches[w]);
    if (run.card && !machine_insert_card(machine, run.card)) { fprintf(stderr, "cannot open card image %s\n", run.card); return 1; }
    if (run.disk && !machine_insert_disk(machine, run.disk, false)) { fprintf(stderr, "cannot open disk image %s\n", run.disk); return 1; }
    if (run.gdb_process && !run.gdb_port) {
        fprintf(stderr, "headless: --gdb-process needs --gdb\n");
        return 2;
    }
    if (run.gdb_port) {
        debugger = gdb_create(machine, run.gdb_port, false, log_stderr);
        if (!debugger) { fprintf(stderr, "cannot listen for GDB on port %d\n", run.gdb_port); return 1; }
        if (run.gdb_process && !gdb_set_process(debugger, run.gdb_process)) fprintf(stderr, "gdb: waiting for %s to start\n", run.gdb_process);
        fprintf(stderr, "gdb: waiting for a connection: target remote :%d\n", run.gdb_port);
        gdb_wait_for_client(debugger);
    }
    screen_size_t screen = machine_screen_size(machine);
    for (int t = 0; t < run.tap_count; t++) {
        if (run.tap_x[t] >= screen.width || run.tap_y[t] >= screen.height) {
            fprintf(stderr, "--tap position %d,%d is off the %ux%u screen\n", run.tap_x[t], run.tap_y[t], screen.width, screen.height);
            return 2;
        }
    }

    FILE *wav_file = run.wav ? fopen(run.wav, "wb") : NULL;
    uint32_t wav_rate = 0;
    size_t wav_samples = 0;
    static int16_t audio[65536];
    if (wav_file) fseek(wav_file, 44, SEEK_SET);

    signal(SIGTERM, request_stop);
    signal(SIGINT, request_stop);
    double wall_start = wall_seconds();
    uint64_t cycles_start = machine_cycles(machine);
    uint64_t total = (uint64_t)(run.seconds * MACHINE_CLOCK_HZ);
    uint64_t slice = MACHINE_CLOCK_HZ / 10;
    for (uint64_t done = 0; done < total && !stop_requested; done += slice) {
        for (int k = 0; k < run.key_count; k++) {
            uint64_t at = (uint64_t)(run.key_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                for (int i = 0; i < run.key_lengths[k]; i++) {
                    if (i) advance(machine, MACHINE_CLOCK_HZ / 50);
                    machine_key(machine, (uint8_t)run.key_codes[k][i], false);
                }
                advance(machine, MACHINE_CLOCK_HZ / 20);
                for (int i = run.key_lengths[k] - 1; i >= 0; i--) {
                    machine_key(machine, (uint8_t)run.key_codes[k][i], true);
                    if (i) advance(machine, MACHINE_CLOCK_HZ / 50);
                }
            }
        }
        for (int t = 0; t < run.tap_count; t++) {
            uint64_t at = (uint64_t)(run.tap_times[t] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_touch(machine, true, run.tap_x[t], run.tap_y[t]);
                advance(machine, (uint64_t)(run.tap_hold[t] * MACHINE_CLOCK_HZ));
                machine_touch(machine, false, run.tap_x[t], run.tap_y[t]);
            }
        }
        if (run.net_at >= 0 && serial.mode == SERIAL_OFF && (uint64_t)(run.net_at * MACHINE_CLOCK_HZ) < done + slice) {
            serial.options = run.net_options;
            serial_link_open(&serial, SERIAL_NETWORK, NULL);
            machine_serial_connect(machine, true);
        }
        if (run.replug_at >= 0 && serial.mode != SERIAL_OFF && (uint64_t)(run.replug_at * MACHINE_CLOCK_HZ) < done + slice) {
            serial_link_close(&serial);
            machine_serial_connect(machine, false);
            run.net_at = run.replug_at + 2;
            run.replug_at = -1;
        }
        if (run.cable_at >= 0 && (uint64_t)(run.cable_at * MACHINE_CLOCK_HZ) >= done && (uint64_t)(run.cable_at * MACHINE_CLOCK_HZ) < done + slice) machine_serial_connect(machine, true);
        for (int k = 0; k < run.send_count; k++) {
            uint64_t at = (uint64_t)(run.send_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                char text[512];
                size_t length = 0;
                for (const char *c = run.send_text[k]; *c && length < sizeof text - 1; c++) {
                    if (c[0] == '\\' && c[1] == 'r') { text[length++] = '\r'; c++; }
                    else if (c[0] == '\\' && c[1] == 'n') { text[length++] = '\n'; c++; }
                    else text[length++] = *c;
                }
                machine_serial_send(machine, (const uint8_t *)text, length);
            }
        }
        for (int k = 0; k < run.type_count; k++) {
            uint64_t at = (uint64_t)(run.type_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) type_text(machine, run.type_strings[k]);
        }
        for (int b = 0; b < run.backlight_count; b++) {
            uint64_t at = (uint64_t)(run.backlight_times[b] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_backlight_button(machine, true);
                advance(machine, MACHINE_CLOCK_HZ / 10);
                machine_backlight_button(machine, false);
            }
        }
        for (int k = 0; k < run.disk_insert_count; k++) {
            uint64_t at = (uint64_t)(run.disk_insert_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice && !machine_insert_disk(machine, run.disk_insert_images[k], false)) fprintf(stderr, "cannot open disk image %s\n", run.disk_insert_images[k]);
        }
        for (int k = 0; k < run.disk_eject_count; k++) {
            uint64_t at = (uint64_t)(run.disk_eject_times[k] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) machine_eject_disk(machine);
        }
        if (run.soft_reset_at >= 0 && (uint64_t)(run.soft_reset_at * MACHINE_CLOCK_HZ) >= done && (uint64_t)(run.soft_reset_at * MACHINE_CLOCK_HZ) < done + slice) machine_soft_reset(machine);
        for (int b = 0; b < run.power_count; b++) {
            uint64_t at = (uint64_t)(run.power_times[b] * MACHINE_CLOCK_HZ);
            if (at >= done && at < done + slice) {
                machine_power_button(machine, true);
                advance(machine, MACHINE_CLOCK_HZ / 5);
                machine_power_button(machine, false);
            }
        }
        if (serial.mode != SERIAL_OFF) {
            uint64_t step = MACHINE_CLOCK_HZ / 100;
            for (uint64_t ran = 0; ran < slice; ran += step) {
                advance(machine, step);
                pace(machine, run.realtime, wall_start, cycles_start);
                serial_link_pump(&serial, machine);
                if (serial.mode == SERIAL_TCP && serial_link_attached(&serial) != machine_serial_connected(machine)) machine_serial_connect(machine, serial_link_attached(&serial));
            }
        } else {
            advance(machine, slice);
            pace(machine, run.realtime, wall_start, cycles_start);
        }
        {
            uint8_t tx[4096];
            size_t count = machine_serial_take(machine, tx, sizeof tx);
            if (count) {
                fprintf(stderr, "SERIAL TX %zu bytes at %u baud t=%.1f:", count, machine_serial_baud(machine), (double)machine_cycles(machine) / MACHINE_CLOCK_HZ);
                for (size_t i = 0; i < count && i < 64; i++) fprintf(stderr, " %02X", tx[i]);
                fprintf(stderr, "\n");
            }
        }
        if (wav_file) {
            uint32_t rate;
            size_t count = machine_audio(machine, audio, sizeof audio / sizeof audio[0], &rate);
            if (count && !wav_rate) wav_rate = rate;
            fwrite(audio, sizeof audio[0], count, wav_file);
            wav_samples += count;
        }
        if (run.trace_pc) fprintf(stderr, "t=%.1fs pc=%08X lcd=%d backlight=%d\n", (double)machine_cycles(machine) / MACHINE_CLOCK_HZ, machine_pc(machine), machine_lcd_enabled(machine), machine_backlight(machine));
    }
    machine_dump_state(machine);
    if (wav_file) {
        uint32_t rate = wav_rate ? wav_rate : 22050, data = (uint32_t)(wav_samples * 2), riff = data + 36, fmt = 16, byte_rate = rate * 2;
        uint16_t pcm = 1, channels = 1, align = 2, bits = 16;
        fseek(wav_file, 0, SEEK_SET);
        fwrite("RIFF", 1, 4, wav_file); fwrite(&riff, 4, 1, wav_file); fwrite("WAVEfmt ", 1, 8, wav_file);
        fwrite(&fmt, 4, 1, wav_file); fwrite(&pcm, 2, 1, wav_file); fwrite(&channels, 2, 1, wav_file);
        fwrite(&rate, 4, 1, wav_file); fwrite(&byte_rate, 4, 1, wav_file); fwrite(&align, 2, 1, wav_file);
        fwrite(&bits, 2, 1, wav_file); fwrite("data", 1, 4, wav_file); fwrite(&data, 4, 1, wav_file);
        fclose(wav_file);
        fprintf(stderr, "wav: %zu samples at %u Hz\n", wav_samples, rate);
    }
    if (run.verify_optimisations) {
        uint32_t checked, differed;
        machine_optimiser_verified(machine, &checked, &differed);
        fprintf(stderr, "optimiser: %u calls checked, %u differed\n", checked, differed);
    }
    if (run.save && !machine_save(machine, run.save, 0)) { fprintf(stderr, "cannot save state %s\n", run.save); return 1; }
    if (run.pgm) {
        static uint8_t levels[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];
        machine_screen(machine, levels);
        write_pgm(run.pgm, levels, machine_screen_size(machine));
    }
    if (run.png && !write_panel_png(run.png, machine, run.png_cell, run.png_backlight)) { fprintf(stderr, "cannot write %s\n", run.png); return 1; }
    serial_link_close(&serial);
    gdb_destroy(debugger);
    agent_destroy(agent);
    machine_destroy(machine);
    free(rom);
    return 0;
}

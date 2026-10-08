#include "app/launch.h"

#include <stdio.h>
#include <string.h>

#include "core/screen.h"

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
    case LAUNCH_VERBOSE: launch->verbose = true; return true;
    case LAUNCH_DEBUG_OUTPUT: launch->debug_output = true; return true;
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

options_result_t launch_parse(launch_t *launch, settings_t *settings, int argc, char **argv) {
    *launch = (launch_t){ .settings = settings, .serial_mode = settings->connect_at_launch ? SERIAL_NETWORK : SERIAL_OFF };
    const char *positional[1];
    int positional_count;
    options_result_t parsed = options_parse(&LAUNCH_SPEC, argc, argv, launch_option, launch, positional, 1, &positional_count);
    if (parsed == OPTIONS_OK && positional_count) launch->rom = positional[0];
    return parsed;
}

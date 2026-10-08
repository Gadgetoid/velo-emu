#pragma once

#include <stdbool.h>

#include "app/settings.h"
#include "net/serial_link.h"
#include "util/options.h"

typedef struct {
    settings_t   *settings;
    serial_mode_t serial_mode;
    const char   *rom, *card, *disk, *state_file, *machine;
    bool fresh, verbose, debug_output;
    int gdb_port;
    const char   *gdb_process;
    const char   *agent_socket;
} launch_t;

options_result_t launch_parse(launch_t *launch, settings_t *settings, int argc, char **argv);

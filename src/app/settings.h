#pragma once

#include <stddef.h>
#include <stdint.h>

#include "core/screen.h"

#define GDB_DEFAULT_PORT  1234
#define RAPI_DEFAULT_PORT 9990
#define SERIAL_TCP_DEFAULT_PORT 9991
#define SETTINGS_SCALE_COUNT 5

typedef struct {
    uint32_t memory;
    screen_size_t screen;
    uint32_t speed;
    uint32_t optimisations;
    uint32_t host_time;
    uint32_t scale;
    uint32_t connect_at_launch;
    uint32_t system;
    char machine[64];
    char serial_device[1024];
    uint32_t display;
    char user_agent[256];
    char shared_folder[1024];
    uint32_t full_brightness;
    uint32_t gdb_server, gdb_port;
    uint32_t network_rapi, rapi_port;
    uint32_t serial_tcp_port;
} settings_t;

settings_t settings_load(void);
void       settings_save(const settings_t *settings);
int        settings_scale_index(uint32_t scale);
uint32_t   settings_scale_at(int index);
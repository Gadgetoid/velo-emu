#include "app/settings.h"

#include <stdio.h>
#include <string.h>

#include "app/paths.h"
#include "app/view.h"
#include "net/net_gateway.h"

#ifdef __ANDROID__
#define DEFAULT_OPTIMISATIONS 1
#else
#define DEFAULT_OPTIMISATIONS 0
#endif

static const uint32_t SCALES[SETTINGS_SCALE_COUNT] = { 50, 75, 100, 150, 200 };

int settings_scale_index(uint32_t scale) {
    for (int i = 0; i < SETTINGS_SCALE_COUNT; i++) {
        if (SCALES[i] == scale) return i;
    }
    return -1;
}

uint32_t settings_scale_at(int index) {
    return SCALES[index];
}

static void copy_setting(char *destination, size_t size, const char *value) {
    size_t length = strcspn(value, "\r\n");
    if (length >= size) length = size - 1;
    memcpy(destination, value, length);
    destination[length] = 0;
}

settings_t settings_load(void) {
    settings_t settings = { .memory = 4, .screen = { SCREEN_STOCK_WIDTH, SCREEN_STOCK_HEIGHT }, .speed = 1, .optimisations = DEFAULT_OPTIMISATIONS, .host_time = 1, .scale = 100, .display = VIEW_SIMULATED, .user_agent = NET_GATEWAY_DEFAULT_USER_AGENT, .full_brightness = 1, .gdb_port = GDB_DEFAULT_PORT, .rapi_port = RAPI_DEFAULT_PORT, .serial_tcp_port = SERIAL_TCP_DEFAULT_PORT };
    char path[1100];
    app_settings_path(path, sizeof path);
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
        else if (sscanf(line, "optimisations=%u", &value) == 1) settings.optimisations = value != 0;
        else if (sscanf(line, "host_time=%u", &value) == 1) settings.host_time = value;
        else if (sscanf(line, "scale=%u", &value) == 1 && settings_scale_index(value) >= 0) settings.scale = value;
        else if (sscanf(line, "connect_at_launch=%u", &value) == 1) settings.connect_at_launch = value;
        else if (sscanf(line, "system=%u", &value) == 1) settings.system = value;
        else if (!strncmp(line, "machine=", 8)) copy_setting(settings.machine, sizeof settings.machine, line + 8);
        else if (sscanf(line, "display=%u", &value) == 1 && value <= VIEW_SHARP) settings.display = value;
        else if (!strncmp(line, "user_agent=", 11)) copy_setting(settings.user_agent, sizeof settings.user_agent, line + 11);
        else if (!strncmp(line, "serial_device=", 14)) copy_setting(settings.serial_device, sizeof settings.serial_device, line + 14);
        else if (!strncmp(line, "shared_folder=", 14)) copy_setting(settings.shared_folder, sizeof settings.shared_folder, line + 14);
        else if (sscanf(line, "full_brightness=%u", &value) == 1) settings.full_brightness = value != 0;
        else if (sscanf(line, "gdb_server=%u", &value) == 1) settings.gdb_server = value != 0;
        else if (sscanf(line, "gdb_port=%u", &value) == 1 && value > 0 && value < 65536) settings.gdb_port = value;
        else if (sscanf(line, "network_rapi=%u", &value) == 1) settings.network_rapi = value != 0;
        else if (sscanf(line, "rapi_port=%u", &value) == 1 && value > 0 && value < 65536) settings.rapi_port = value;
        else if (sscanf(line, "serial_tcp_port=%u", &value) == 1 && value > 0 && value < 65536) settings.serial_tcp_port = value;
    }
    fclose(file);
    return settings;
}

void settings_save(const settings_t *settings) {
    char path[1100];
    app_settings_path(path, sizeof path);
    FILE *file = fopen(path, "w");
    if (!file) return;
    fprintf(file, "optimisations=%u\n", settings->optimisations);
    fprintf(file, "memory=%u\nscreen=%ux%u\nspeed=%u\nhost_time=%u\nscale=%u\ndisplay=%u\nconnect_at_launch=%u\nsystem=%u\nmachine=%s\nserial_device=%s\nuser_agent=%s\nshared_folder=%s\nfull_brightness=%u\ngdb_server=%u\ngdb_port=%u\nnetwork_rapi=%u\nrapi_port=%u\nserial_tcp_port=%u\n", settings->memory,
            settings->screen.width, settings->screen.height, settings->speed, settings->host_time, settings->scale, settings->display, settings->connect_at_launch, settings->system, settings->machine, settings->serial_device,
            settings->user_agent, settings->shared_folder, settings->full_brightness, settings->gdb_server, settings->gdb_port,
            settings->network_rapi, settings->rapi_port, settings->serial_tcp_port);
    fclose(file);
}
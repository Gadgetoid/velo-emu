#include "app/paths.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "net/net_gateway.h"
#include "rapi/rapi.h"

void app_data_folder(char *path, size_t size) {
    rapi_data_path("", path, size);
    size_t length = strlen(path);
    if (length > 1 && path[length - 1] == '/') path[length - 1] = 0;
    SDL_CreateDirectory(path);
}

void app_paths_migrate_old_folders(void) {
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

void app_settings_path(char *path, size_t size) {
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char base[1024];
    if (config_home && config_home[0] == '/') snprintf(base, sizeof base, "%s/velo-emu", config_home);
#ifdef __APPLE__
    else app_data_folder(base, sizeof base);
#else
    else snprintf(base, sizeof base, "%s/.config/velo-emu", getenv("HOME") ? getenv("HOME") : ".");
#endif
    SDL_CreateDirectory(base);
    snprintf(path, size, "%s/emu.ini", base);
}

void app_rapi_socket_path(char *path, size_t size) {
#ifdef __ANDROID__
    net_gateway_socket_path(path, size, "velo-rapi");
#else
    rapi_data_path("rapi.sock", path, size);
#endif
}

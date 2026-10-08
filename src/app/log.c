#include "app/log.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "app/paths.h"
#include "util/file.h"

#define DEBUG_LOG_MAX (1024 * 1024)

static bool verbose;
static FILE *debug_log;
static bool debug_to_stderr;

void app_log_set_verbose(bool enabled) {
    verbose = enabled;
}

bool app_log_verbose(void) {
    return verbose;
}

void app_log(const char *message) {
#ifdef __ANDROID__
    SDL_Log("%s", message);
#endif
    if (verbose) fputs(message, stderr);
}

void app_log_always(const char *message) {
#ifdef __ANDROID__
    SDL_Log("%s", message);
#endif
    fputs(message, stderr);
}

void debug_log_set_stderr(bool enabled) {
    debug_to_stderr = enabled;
}

void debug_log_path(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/debug.log", base);
}

void debug_log_start(const char *rom_path) {
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

void debug_log_line(const char *line) {
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

void debug_log_flush(void) {
    if (debug_log) fflush(debug_log);
}

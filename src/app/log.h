#pragma once

#include <stdbool.h>
#include <stddef.h>

void app_log_set_verbose(bool verbose);
bool app_log_verbose(void);
void app_log(const char *message);
void app_log_always(const char *message);

void debug_log_set_stderr(bool enabled);
void debug_log_path(char *path, size_t size);
void debug_log_start(const char *rom_path);
void debug_log_line(const char *line);
void debug_log_flush(void);

#pragma once

#include <stddef.h>

void app_data_folder(char *path, size_t size);
void app_settings_path(char *path, size_t size);
void app_rapi_socket_path(char *path, size_t size);
void app_paths_migrate_old_folders(void);
#pragma once
#include <stdbool.h>
#include <stddef.h>

void android_update(bool backlit, bool awake);
bool android_local_path(const char *uri, const char *folder, char *path, size_t size);
bool android_import(const char *uri, const char *folder, char *path, size_t size);
bool android_export(const char *path, const char *uri);

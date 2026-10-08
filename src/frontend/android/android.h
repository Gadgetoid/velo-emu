#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void android_update(bool backlit, bool awake);
bool android_local_path(const char *uri, const char *folder, char *path, size_t size);
bool android_import(const char *uri, const char *folder, char *path, size_t size);
bool android_export(const char *path, const char *uri);
bool android_save_picture(const uint8_t *png, size_t length, const char *name);
bool android_share_picture(const uint8_t *png, size_t length, const char *name);
bool android_all_files_access(void);
void android_request_all_files_access(void);

void android_progress(const char *title, float fraction);
bool android_toast(const char *text);
bool android_choose_folder(const char *title, const char *start, char *path, size_t size);

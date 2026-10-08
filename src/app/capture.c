#include "app/capture.h"

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app/android.h"
#include "util/png.h"

#ifdef __APPLE__
#define SCREENSHOT_FOLDER SDL_FOLDER_DESKTOP
#else
#define SCREENSHOT_FOLDER SDL_FOLDER_PICTURES
#endif

static const void *clipboard_png(void *userdata, const char *mime_type, size_t *size) {
    const size_t *stored = userdata;
    if (strcmp(mime_type, "image/png")) { *size = 0; return NULL; }
    *size = stored[0];
    return stored + 1;
}

bool capture_copy_screen(view_t *view) {
    int width, height;
    const uint32_t *pixels = view_image(view, &width, &height);
    uint8_t *png;
    size_t length;
    if (!pixels || !png_encode(pixels, width, height, &png, &length)) return false;
#ifdef __ANDROID__
    bool shared = android_share_picture(png, length, "Velo Screen.png");
    free(png);
    return shared;
#endif
    size_t *stored = malloc(sizeof(size_t) + length);
    if (!stored) { free(png); return false; }
    stored[0] = length;
    memcpy(stored + 1, png, length);
    free(png);
    const char *types[] = { "image/png" };
    if (SDL_SetClipboardData(clipboard_png, free, stored, types, 1)) return true;
    free(stored);
    return false;
}

bool capture_save_screenshot(view_t *view, char *path, size_t size) {
    int width, height;
    const uint32_t *pixels = view_image(view, &width, &height);
    uint8_t *png;
    size_t length;
    if (!pixels || !png_encode(pixels, width, height, &png, &length)) return false;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
#ifdef __ANDROID__
    snprintf(path, size, "Velo Screenshot %s.png", stamp);
    bool stored = android_save_picture(png, length, path);
    free(png);
    return stored;
#endif
    const char *folder = SDL_GetUserFolder(SCREENSHOT_FOLDER);
    if (folder) snprintf(path, size, "%sVelo Screenshot %s.png", folder, stamp);
    else snprintf(path, size, "%s/Velo Screenshot %s.png", getenv("HOME") ? getenv("HOME") : ".", stamp);
    FILE *file = fopen(path, "wb");
    bool saved = file && fwrite(png, 1, length, file) == length;
    if (file) fclose(file);
    free(png);
    return saved;
}

#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "app/android.h"

#define COMMAND_BRIGHTNESS     0x8000
#define COMMAND_KEEP_SCREEN_ON 0x8001
#define FULL_BRIGHTNESS        1000
#define SYSTEM_BRIGHTNESS      (-1)
#define COPY_CHUNK             65536

void android_update(bool backlit, bool awake) {
    static int brightness = 0, keep_screen_on = -1;
    int wanted_brightness = backlit ? FULL_BRIGHTNESS : SYSTEM_BRIGHTNESS;
    if (wanted_brightness != brightness && SDL_SendAndroidMessage(COMMAND_BRIGHTNESS, wanted_brightness)) brightness = wanted_brightness;
    if ((int)awake != keep_screen_on && SDL_SendAndroidMessage(COMMAND_KEEP_SCREEN_ON, awake)) keep_screen_on = awake;
}

static bool display_name(const char *uri, char *name, size_t size) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    jobject activity = SDL_GetAndroidActivity();
    if (!env || !activity) return false;
    bool found = false;
    jclass class = (*env)->GetObjectClass(env, activity);
    jmethodID method = (*env)->GetMethodID(env, class, "displayName", "(Ljava/lang/String;)Ljava/lang/String;");
    if (method) {
        jstring text = (*env)->NewStringUTF(env, uri);
        jstring result = (jstring)(*env)->CallObjectMethod(env, activity, method, text);
        if ((*env)->ExceptionCheck(env)) {
            (*env)->ExceptionClear(env);
        } else if (result) {
            const char *utf = (*env)->GetStringUTFChars(env, result, NULL);
            if (utf) {
                SDL_strlcpy(name, utf, size);
                found = name[0] != 0;
                (*env)->ReleaseStringUTFChars(env, result, utf);
            }
            (*env)->DeleteLocalRef(env, result);
        }
        (*env)->DeleteLocalRef(env, text);
    } else {
        (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, class);
    (*env)->DeleteLocalRef(env, activity);
    return found;
}

bool android_local_path(const char *uri, const char *folder, char *path, size_t size) {
    char name[256];
    if (!display_name(uri, name, sizeof name)) {
        const char *slash = strrchr(uri, '/');
        SDL_strlcpy(name, slash && slash[1] ? slash + 1 : "imported", sizeof name);
    }
    for (char *at = name; *at; at++) {
        if (*at == '/' || *at == ':' || *at == '%') *at = '_';
    }
    int length = snprintf(path, size, "%s/%s", folder, name);
    return length > 0 && (size_t)length < size;
}

static bool copy_stream(SDL_IOStream *from, SDL_IOStream *to) {
    static char buffer[COPY_CHUNK];
    bool ok = from && to;
    while (ok) {
        size_t read = SDL_ReadIO(from, buffer, sizeof buffer);
        if (!read) {
            ok = SDL_GetIOStatus(from) == SDL_IO_STATUS_EOF;
            break;
        }
        ok = SDL_WriteIO(to, buffer, read) == read;
    }
    if (from) SDL_CloseIO(from);
    if (to && !SDL_CloseIO(to)) ok = false;
    return ok;
}

bool android_import(const char *uri, const char *folder, char *path, size_t size) {
    if (!android_local_path(uri, folder, path, size)) return false;
    return copy_stream(SDL_IOFromFile(uri, "rb"), SDL_IOFromFile(path, "wb"));
}

bool android_export(const char *path, const char *uri) {
    return copy_stream(SDL_IOFromFile(path, "rb"), SDL_IOFromFile(uri, "wb"));
}

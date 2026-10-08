#include <SDL3/SDL.h>
#include <jni.h>
#include <stdio.h>
#include <string.h>

#include "frontend/android/android.h"

#define COMMAND_BRIGHTNESS     0x8000
#define COMMAND_KEEP_SCREEN_ON 0x8001
#define FULL_BRIGHTNESS        1000
#define SYSTEM_BRIGHTNESS      (-1)
#define COPY_CHUNK             65536
#define PROGRESS_STEP          (4 * 1024 * 1024)

void android_update(bool backlit, bool awake) {
    static int brightness = 0, keep_screen_on = -1;
    int wanted_brightness = backlit ? FULL_BRIGHTNESS : SYSTEM_BRIGHTNESS;
    if (wanted_brightness != brightness && SDL_SendAndroidMessage(COMMAND_BRIGHTNESS, wanted_brightness)) brightness = wanted_brightness;
    if ((int)awake != keep_screen_on && SDL_SendAndroidMessage(COMMAND_KEEP_SCREEN_ON, awake)) keep_screen_on = awake;
}

typedef struct {
    JNIEnv   *env;
    jobject activity;
    jclass class;
    jmethodID method;
} java_call_t;

static bool java_begin(java_call_t *call, const char *name, const char *signature) {
    call->env = SDL_GetAndroidJNIEnv();
    call->activity = call->env ? SDL_GetAndroidActivity() : NULL;
    if (!call->activity) return false;
    JNIEnv *env = call->env;
    call->class = (*env)->GetObjectClass(env, call->activity);
    call->method = (*env)->GetMethodID(env, call->class, name, signature);
    if (call->method) return true;
    (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, call->class);
    (*env)->DeleteLocalRef(env, call->activity);
    return false;
}

static void java_end(java_call_t *call) {
    JNIEnv *env = call->env;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, call->class);
    (*env)->DeleteLocalRef(env, call->activity);
}

static bool picture_call(const char *method, const uint8_t *png, size_t length, const char *name) {
    java_call_t call;
    if (!java_begin(&call, method, "([BLjava/lang/String;)Z")) return false;
    JNIEnv *env = call.env;
    jbyteArray bytes = (*env)->NewByteArray(env, (jsize)length);
    jstring text = (*env)->NewStringUTF(env, name);
    bool ok = false;
    if (bytes && text) {
        (*env)->SetByteArrayRegion(env, bytes, 0, (jsize)length, (const jbyte *)png);
        ok = (*env)->CallBooleanMethod(env, call.activity, call.method, bytes, text) && !(*env)->ExceptionCheck(env);
    }
    if (bytes) (*env)->DeleteLocalRef(env, bytes);
    if (text) (*env)->DeleteLocalRef(env, text);
    java_end(&call);
    return ok;
}

bool android_save_picture(const uint8_t *png, size_t length, const char *name) {
    return picture_call("savePicture", png, length, name);
}

bool android_share_picture(const uint8_t *png, size_t length, const char *name) {
    return picture_call("sharePicture", png, length, name);
}

bool android_all_files_access(void) {
    java_call_t call;
    if (!java_begin(&call, "hasAllFilesAccess", "()Z")) return false;
    bool granted = (*call.env)->CallBooleanMethod(call.env, call.activity, call.method) && !(*call.env)->ExceptionCheck(call.env);
    java_end(&call);
    return granted;
}

void android_request_all_files_access(void) {
    java_call_t call;
    if (!java_begin(&call, "requestAllFilesAccess", "()V")) return;
    (*call.env)->CallVoidMethod(call.env, call.activity, call.method);
    java_end(&call);
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

static bool copy_stream(SDL_IOStream *from, SDL_IOStream *to, const char *title) {
    static char buffer[COPY_CHUNK];
    bool ok = from && to;
    Sint64 total = from ? SDL_GetIOSize(from) : -1;
    Sint64 copied = 0, reported = 0;
    if (ok) android_progress(title, total > 0 ? 0.0f : -1.0f);
    while (ok) {
        size_t read = SDL_ReadIO(from, buffer, sizeof buffer);
        if (!read) {
            ok = SDL_GetIOStatus(from) == SDL_IO_STATUS_EOF;
            break;
        }
        ok = SDL_WriteIO(to, buffer, read) == read;
        copied += (Sint64)read;
        if (copied - reported >= PROGRESS_STEP) {
            reported = copied;
            android_progress(title, total > 0 ? (float)copied / (float)total : -1.0f);
        }
    }
    if (from) SDL_CloseIO(from);
    if (to && !SDL_CloseIO(to)) ok = false;
    return ok;
}

static const char *leaf(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool android_import(const char *uri, const char *folder, char *path, size_t size) {
    if (!android_local_path(uri, folder, path, size)) return false;
    char title[300];
    snprintf(title, sizeof title, "Copying %s", leaf(path));
    return copy_stream(SDL_IOFromFile(uri, "rb"), SDL_IOFromFile(path, "wb"), title);
}

bool android_export(const char *path, const char *uri) {
    char title[300];
    snprintf(title, sizeof title, "Saving %s", leaf(path));
    return copy_stream(SDL_IOFromFile(path, "rb"), SDL_IOFromFile(uri, "wb"), title);
}

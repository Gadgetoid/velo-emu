#include "app/desktop.h"
#include "rapi/rapi.h"
#include "rapi/rapi_load.h"
#include "rapi/rapi_setup.h"
#include "rapi/rapi_sync.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REMOTE_HOME      "\\My Documents"
#define CONNECT_ATTEMPTS 10
#define PATH_SIZE        1024

typedef enum { JOB_SEND, JOB_FETCH, JOB_SYNC, JOB_PROXY, JOB_BAUD, JOB_LOAD } job_kind_t;

struct desktop {
    char            socket_path[PATH_SIZE];
    char            manifest_path[PATH_SIZE];
    pthread_mutex_t lock;
    bool            busy, status_changed, reconnect;
    unsigned        baud;
    char            status[256];
    job_kind_t      kind;
    char          **files;
    size_t          file_count;
    char            folder[PATH_SIZE];
};

typedef struct {
    desktop_t  *desktop;
    const char *name;
    size_t      index, count;
} progress_t;

static void set_status(desktop_t *desktop, const char *format, ...) {
    pthread_mutex_lock(&desktop->lock);
    va_list args;
    va_start(args, format);
    vsnprintf(desktop->status, sizeof desktop->status, format, args);
    va_end(args);
    desktop->status_changed = true;
    pthread_mutex_unlock(&desktop->lock);
}

static void show_progress(void *context, uint64_t done, uint64_t total) {
    progress_t *progress = context;
    unsigned percent = total ? (unsigned)(done * 100 / total) : 100;
    if (progress->count > 1) set_status(progress->desktop, "%s (%zu of %zu) %u%%", progress->name, progress->index + 1, progress->count, percent);
    else set_status(progress->desktop, "%s %u%%", progress->name, percent);
}

static void sync_message(void *context, const char *message) {
    set_status(context, "%s", message);
}

static rapi_t *connect_velo(desktop_t *desktop) {
    char error[PATH_SIZE + 128] = "";
    for (int attempt = 0; attempt < CONNECT_ATTEMPTS; attempt++) {
        rapi_t *rapi = rapi_connect(desktop->socket_path, error, sizeof error);
        if (rapi) return rapi;
        sleep(1);
    }
    set_status(desktop, "%s", error);
    return NULL;
}

static const char *leaf_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void run_send(desktop_t *desktop, rapi_t *rapi) {
    size_t sent = 0;
    for (size_t i = 0; i < desktop->file_count; i++) {
        const char *name = leaf_of(desktop->files[i]);
        char remote[PATH_SIZE];
        snprintf(remote, sizeof remote, "%s\\%s", REMOTE_HOME, name);
        progress_t progress = { desktop, name, i, desktop->file_count };
        if (!rapi_upload(rapi, desktop->files[i], remote, show_progress, &progress)) {
            set_status(desktop, "%s", rapi_error(rapi));
            return;
        }
        sent++;
    }
    set_status(desktop, "sent %zu file%s to \\My Documents", sent, sent == 1 ? "" : "s");
}

static bool fetch_folder(desktop_t *desktop, rapi_t *rapi, const char *remote, const char *local, int depth, size_t *copied) {
    if (depth > RAPI_FOLDER_DEPTH_MAX) {
        set_status(desktop, "can't copy %s: folders nested too deeply", remote);
        return false;
    }
    char pattern[PATH_SIZE * 2];
    snprintf(pattern, sizeof pattern, "%s\\*", remote);
    rapi_file_t *files;
    size_t count;
    if (!rapi_list(rapi, pattern, &files, &count)) {
        set_status(desktop, "%s", rapi_error(rapi));
        return false;
    }
    mkdir(local, 0755);
    bool success = true;
    for (size_t i = 0; i < count && success; i++) {
        char child_remote[PATH_SIZE * 2], child_local[PATH_SIZE * 2];
        snprintf(child_remote, sizeof child_remote, "%s\\%s", remote, files[i].name);
        snprintf(child_local, sizeof child_local, "%s/%s", local, files[i].name);
        if (files[i].attributes & RAPI_ATTRIBUTE_DIRECTORY) {
            success = fetch_folder(desktop, rapi, child_remote, child_local, depth + 1, copied);
            continue;
        }
        progress_t progress = { desktop, files[i].name, 0, 1 };
        success = rapi_download(rapi, child_remote, child_local, show_progress, &progress);
        if (success) (*copied)++;
        else set_status(desktop, "%s", rapi_error(rapi));
    }
    free(files);
    return success;
}

static void run_fetch(desktop_t *desktop, rapi_t *rapi) {
    size_t copied = 0;
    if (fetch_folder(desktop, rapi, REMOTE_HOME, desktop->folder, 0, &copied)) {
        set_status(desktop, "copied %zu file%s from \\My Documents", copied, copied == 1 ? "" : "s");
    }
}

static void run_sync(desktop_t *desktop, rapi_t *rapi) {
    set_status(desktop, "syncing %s", leaf_of(desktop->folder));
    rapi_sync_result_t result;
    if (!rapi_sync_run(rapi, desktop->folder, REMOTE_HOME, desktop->manifest_path, sync_message, desktop, &result)) {
        set_status(desktop, "sync stopped: %s", rapi_error(rapi));
        return;
    }
    unsigned changes = result.uploaded + result.downloaded + result.deleted_on_mac + result.deleted_on_velo;
    if (!changes && !result.skipped) set_status(desktop, "%s is up to date", leaf_of(desktop->folder));
    else set_status(desktop, "synced: %u to the Velo, %u from it, %u deleted%s%s", result.uploaded, result.downloaded,
                    result.deleted_on_mac + result.deleted_on_velo, result.conflicts ? ", conflicts kept as (Velo) copies" : "",
                    result.skipped ? ", some skipped" : "");
}

static void run_load(desktop_t *desktop, rapi_t *rapi) {
    if (rapi_load_run(rapi, desktop->folder, NULL, sync_message, desktop)) set_status(desktop, "installed %s", leaf_of(desktop->folder));
    else set_status(desktop, "%s didn't install cleanly, see velo-rapi load for details", leaf_of(desktop->folder));
}

static void run_proxy(desktop_t *desktop, rapi_t *rapi) {
    rapi_version_t version = { 0 };
    if (!rapi_setup_proxy(rapi, true)) set_status(desktop, "%s", rapi_error(rapi));
    else if (rapi_version(rapi, &version) && version.major >= 2) set_status(desktop, "Pocket IE uses the web proxy after a soft reset");
    else set_status(desktop, "Pocket IE uses the web proxy from its next start");
}

static void run_baud(desktop_t *desktop, rapi_t *rapi) {
    if (!rapi_setup_connection(rapi, desktop->baud)) {
        set_status(desktop, "%s", rapi_error(rapi));
        return;
    }
    set_status(desktop, "desktop connection set to %u baud; reconnecting in a few seconds", desktop->baud);
    pthread_mutex_lock(&desktop->lock);
    desktop->reconnect = true;
    pthread_mutex_unlock(&desktop->lock);
}

static void *job_thread(void *opaque) {
    desktop_t *desktop = opaque;
    rapi_t *rapi = connect_velo(desktop);
    if (rapi) {
        if (desktop->kind == JOB_SEND) run_send(desktop, rapi);
        else if (desktop->kind == JOB_FETCH) run_fetch(desktop, rapi);
        else if (desktop->kind == JOB_PROXY) run_proxy(desktop, rapi);
        else if (desktop->kind == JOB_BAUD) run_baud(desktop, rapi);
        else if (desktop->kind == JOB_LOAD) run_load(desktop, rapi);
        else run_sync(desktop, rapi);
        rapi_disconnect(rapi);
    }
    for (size_t i = 0; i < desktop->file_count; i++) free(desktop->files[i]);
    free(desktop->files);
    desktop->files = NULL;
    desktop->file_count = 0;
    pthread_mutex_lock(&desktop->lock);
    desktop->busy = false;
    pthread_mutex_unlock(&desktop->lock);
    return NULL;
}

static bool claim(desktop_t *desktop) {
    pthread_mutex_lock(&desktop->lock);
    bool free_to_start = !desktop->busy;
    if (free_to_start) desktop->busy = true;
    pthread_mutex_unlock(&desktop->lock);
    if (!free_to_start) set_status(desktop, "busy with the last transfer");
    return free_to_start;
}

static bool start(desktop_t *desktop) {
    pthread_t thread;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    bool started = pthread_create(&thread, &attributes, job_thread, desktop) == 0;
    pthread_attr_destroy(&attributes);
    if (!started) {
        pthread_mutex_lock(&desktop->lock);
        desktop->busy = false;
        pthread_mutex_unlock(&desktop->lock);
    }
    return started;
}

desktop_t *desktop_create(const char *socket_path, const char *manifest_path) {
    desktop_t *desktop = calloc(1, sizeof *desktop);
    snprintf(desktop->socket_path, sizeof desktop->socket_path, "%s", socket_path);
    snprintf(desktop->manifest_path, sizeof desktop->manifest_path, "%s", manifest_path);
    pthread_mutex_init(&desktop->lock, NULL);
    return desktop;
}

void desktop_destroy(desktop_t *desktop) {
    if (!desktop || desktop_busy(desktop)) return;
    pthread_mutex_destroy(&desktop->lock);
    free(desktop);
}

bool desktop_busy(desktop_t *desktop) {
    pthread_mutex_lock(&desktop->lock);
    bool busy = desktop->busy;
    pthread_mutex_unlock(&desktop->lock);
    return busy;
}

bool desktop_take_status(desktop_t *desktop, char *text, size_t size) {
    pthread_mutex_lock(&desktop->lock);
    bool changed = desktop->status_changed;
    if (changed) snprintf(text, size, "%s", desktop->status);
    desktop->status_changed = false;
    pthread_mutex_unlock(&desktop->lock);
    return changed;
}

bool desktop_send(desktop_t *desktop, const char *const *files) {
    size_t count = 0;
    while (files[count]) count++;
    if (!count || !claim(desktop)) return false;
    desktop->kind = JOB_SEND;
    desktop->files = calloc(count, sizeof *desktop->files);
    for (size_t i = 0; i < count; i++) desktop->files[i] = strdup(files[i]);
    desktop->file_count = count;
    return start(desktop);
}

bool desktop_fetch(desktop_t *desktop, const char *local_folder) {
    if (!claim(desktop)) return false;
    desktop->kind = JOB_FETCH;
    snprintf(desktop->folder, sizeof desktop->folder, "%s", local_folder);
    return start(desktop);
}

bool desktop_sync(desktop_t *desktop, const char *folder) {
    if (!claim(desktop)) return false;
    desktop->kind = JOB_SYNC;
    snprintf(desktop->folder, sizeof desktop->folder, "%s", folder);
    return start(desktop);
}

bool desktop_load(desktop_t *desktop, const char *script) {
    if (!claim(desktop)) return false;
    desktop->kind = JOB_LOAD;
    snprintf(desktop->folder, sizeof desktop->folder, "%s", script);
    return start(desktop);
}

bool desktop_set_proxy(desktop_t *desktop) {
    if (!claim(desktop)) return false;
    desktop->kind = JOB_PROXY;
    return start(desktop);
}

bool desktop_set_baud(desktop_t *desktop, unsigned baud) {
    if (!claim(desktop)) return false;
    desktop->kind = JOB_BAUD;
    desktop->baud = baud;
    return start(desktop);
}

bool desktop_take_reconnect(desktop_t *desktop) {
    pthread_mutex_lock(&desktop->lock);
    bool reconnect = desktop->reconnect;
    desktop->reconnect = false;
    pthread_mutex_unlock(&desktop->lock);
    return reconnect;
}

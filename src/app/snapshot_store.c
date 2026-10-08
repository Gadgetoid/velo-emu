#include "app/snapshot_store.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "util/file.h"

#define BACKUP_KEEP 10

void snapshot_store_init(snapshot_store_t *store, const char *data_folder) {
    snprintf(store->data_folder, sizeof store->data_folder, "%s", data_folder);
}

void snapshot_store_folder(snapshot_store_t *store, char *path, size_t size) {
    snprintf(path, size, "%s/snapshots", store->data_folder);
    SDL_CreateDirectory(path);
}

static void backup_path(snapshot_store_t *store, const char *state, char *prefix, size_t prefix_size, char *path, size_t size) {
    char folder[1100];
    snapshot_store_folder(store, folder, sizeof folder);
    snprintf(folder + strlen(folder), sizeof folder - strlen(folder), "/Backups");
    SDL_CreateDirectory(folder);
    char name[256];
    snprintf(name, sizeof name, "%s", file_leaf_name(state));
    char *extension = strrchr(name, '.');
    if (extension && extension != name) *extension = 0;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(prefix, prefix_size, "%s ", name);
    snprintf(path, size, "%s/%s%s.state", folder, prefix, stamp);
}

static int compare_name_pointers(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void prune_backups(const char *path, const char *prefix) {
    char folder[1100];
    snprintf(folder, sizeof folder, "%s", path);
    char *slash = strrchr(folder, '/');
    if (!slash) return;
    *slash = 0;
    DIR *dir = opendir(folder);
    if (!dir) return;
    char *names[256];
    int count = 0;
    size_t prefix_length = strlen(prefix);
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < 256) {
        if (strncmp(entry->d_name, prefix, prefix_length) || !file_has_extension(entry->d_name, ".state")) continue;
        names[count] = strdup(entry->d_name);
        if (names[count]) count++;
    }
    closedir(dir);
    qsort(names, (size_t)count, sizeof names[0], compare_name_pointers);
    for (int i = 0; i < count; i++) {
        if (i < count - BACKUP_KEEP) {
            char old[1400];
            snprintf(old, sizeof old, "%s/%s", folder, names[i]);
            remove(old);
        }
        free(names[i]);
    }
}

bool snapshot_store_backup_machine(snapshot_store_t *store, machine_t *machine, const char *state) {
    char prefix[300], path[1400];
    backup_path(store, state, prefix, sizeof prefix, path, sizeof path);
    bool saved = machine_save(machine, path, (int64_t)time(NULL));
    if (saved) prune_backups(path, prefix);
    return saved;
}

bool snapshot_store_backup_file(snapshot_store_t *store, const char *state) {
    size_t size;
    uint8_t *contents = file_read(state, &size);
    if (!contents) return false;
    char prefix[300], path[1400];
    backup_path(store, state, prefix, sizeof prefix, path, sizeof path);
    FILE *file = fopen(path, "wb");
    bool written = file && fwrite(contents, 1, size, file) == size;
    if (file && fclose(file) != 0) written = false;
    free(contents);
    if (written) prune_backups(path, prefix);
    return written;
}

void snapshot_store_default_name(snapshot_store_t *store, char *path, size_t size) {
    char folder[1100];
    snapshot_store_folder(store, folder, sizeof folder);
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char stamp[64];
    strftime(stamp, sizeof stamp, "%Y-%m-%d at %H.%M.%S", &local);
    snprintf(path, size, "%s/Snapshot %s.state", folder, stamp);
}
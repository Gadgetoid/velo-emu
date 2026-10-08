#include "rapi/rapi_sync.h"
#include "rapi/rapi_project.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PATH_SIZE       1024
#define FREE_MARGIN     (64 * 1024)
#define MANIFEST_HEADER RAPI_SYNC_MANIFEST

typedef struct {
    char path[PATH_SIZE];
    bool on_mac, on_device, in_manifest, failed;
    int64_t mac_time;
    uint64_t mac_size;
    uint64_t device_time;
    uint32_t device_size;
    int64_t known_mac_time;
    uint64_t known_mac_size;
    uint64_t known_device_time;
    uint32_t known_device_size;
} entry_t;

typedef struct {
    entry_t *items;
    size_t count, capacity;
} entries_t;

typedef struct {
    rapi_t            *rapi;
    const char        *folder;
    const char        *remote_root;
    rapi_sync_log_fn log;
    void              *context;
    rapi_sync_result_t *result;
    uint64_t free_space;
    bool disconnected;
} sync_t;

static void sync_log(sync_t *sync, const char *format, ...) {
    if (!sync->log) return;
    char message[PATH_SIZE * 2];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    sync->log(sync->context, message);
}

static entry_t *find_or_add(entries_t *entries, const char *path) {
    for (size_t i = 0; i < entries->count; i++) {
        if (!strcasecmp(entries->items[i].path, path)) return &entries->items[i];
    }
    if (entries->count == entries->capacity) {
        size_t capacity = entries->capacity ? entries->capacity * 2 : 64;
        entry_t *grown = realloc(entries->items, capacity * sizeof *grown);
        if (!grown) return NULL;
        entries->items = grown;
        entries->capacity = capacity;
    }
    entry_t *entry = &entries->items[entries->count++];
    memset(entry, 0, sizeof *entry);
    snprintf(entry->path, sizeof entry->path, "%s", path);
    return entry;
}

static void join_relative(char *out, size_t size, const char *relative, const char *name) {
    if (*relative) snprintf(out, size, "%s/%s", relative, name);
    else snprintf(out, size, "%s", name);
}

static void local_path(const sync_t *sync, const char *relative, char *out, size_t size) {
    snprintf(out, size, "%s/%s", sync->folder, relative);
}

static void remote_path(const sync_t *sync, const char *relative, char *out, size_t size) {
    if (*relative) snprintf(out, size, "%s\\%s", sync->remote_root, relative);
    else snprintf(out, size, "%s", sync->remote_root);
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
}

static bool valid_on_device(const char *relative) {
    return strpbrk(relative, "\\:*?\"<>|") == NULL;
}

static void scan_mac(sync_t *sync, entries_t *entries, const char *relative) {
    char directory[PATH_SIZE * 2];
    local_path(sync, relative, directory, sizeof directory);
    DIR *dir = opendir(directory);
    if (!dir) return;
    struct dirent *item;
    while ((item = readdir(dir))) {
        const char *name = item->d_name;
        size_t length = strlen(name);
        if (name[0] == '.' || (length > 5 && !strcmp(name + length - 5, ".part"))) continue;
        char child[PATH_SIZE], full[PATH_SIZE * 2];
        join_relative(child, sizeof child, relative, name);
        local_path(sync, child, full, sizeof full);
        struct stat info;
        if (lstat(full, &info) != 0) continue;
        if (S_ISLNK(info.st_mode) && (stat(full, &info) != 0 || !S_ISREG(info.st_mode))) continue;
        if (S_ISDIR(info.st_mode)) {
            scan_mac(sync, entries, child);
        } else if (S_ISREG(info.st_mode)) {
            entry_t *entry = find_or_add(entries, child);
            if (!entry) continue;
            entry->on_mac = true;
            entry->mac_time = (int64_t)info.st_mtime;
            entry->mac_size = (uint64_t)info.st_size;
        }
    }
    closedir(dir);
}

static bool scan_device(sync_t *sync, entries_t *entries, const char *relative, int depth) {
    if (depth > RAPI_FOLDER_DEPTH_MAX) {
        sync_log(sync, "can't list %s: folders nested too deeply", relative);
        return false;
    }
    char pattern[PATH_SIZE * 2];
    remote_path(sync, relative, pattern, sizeof pattern - 2);
    strcat(pattern, "\\*");
    rapi_file_t *files;
    size_t count;
    if (!rapi_list(sync->rapi, pattern, &files, &count)) {
        sync_log(sync, "can't list %s: %s", pattern, rapi_error(sync->rapi));
        return false;
    }
    bool success = true;
    for (size_t i = 0; i < count && success; i++) {
        char child[PATH_SIZE];
        join_relative(child, sizeof child, relative, files[i].name);
        if (files[i].attributes & RAPI_ATTRIBUTE_DIRECTORY) {
            success = scan_device(sync, entries, child, depth + 1);
            continue;
        }
        entry_t *entry = find_or_add(entries, child);
        if (!entry) continue;
        entry->on_device = true;
        entry->device_time = files[i].write_time;
        entry->device_size = files[i].size;
    }
    free(files);
    return success;
}

static void load_manifest(sync_t *sync, entries_t *entries, const char *manifest_path) {
    FILE *file = fopen(manifest_path, "r");
    if (!file) return;
    char line[PATH_SIZE * 2], header[PATH_SIZE * 2];
    snprintf(header, sizeof header, "%s\t%s\t%s\n", MANIFEST_HEADER, sync->folder, sync->remote_root);
    if (!fgets(line, sizeof line, file) || strcmp(line, header)) {
        fclose(file);
        return;
    }
    while (fgets(line, sizeof line, file)) {
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        long long mac_time;
        unsigned long long mac_size, device_time;
        unsigned device_size;
        if (sscanf(tab + 1, "%lld\t%llu\t%llu\t%u", &mac_time, &mac_size, &device_time, &device_size) != 4) continue;
        entry_t *entry = find_or_add(entries, line);
        if (!entry) continue;
        entry->in_manifest = true;
        entry->known_mac_time = mac_time;
        entry->known_mac_size = mac_size;
        entry->known_device_time = device_time;
        entry->known_device_size = device_size;
    }
    fclose(file);
}

static void save_manifest(sync_t *sync, const entries_t *entries, const char *manifest_path) {
    char partial[PATH_SIZE * 2];
    snprintf(partial, sizeof partial, "%s.new", manifest_path);
    FILE *file = fopen(partial, "w");
    if (!file) return;
    fprintf(file, "%s\t%s\t%s\n", MANIFEST_HEADER, sync->folder, sync->remote_root);
    for (size_t i = 0; i < entries->count; i++) {
        const entry_t *entry = &entries->items[i];
        if (entry->failed && entry->in_manifest) {
            fprintf(file, "%s\t%lld\t%llu\t%llu\t%u\n", entry->path, (long long)entry->known_mac_time,
                    (unsigned long long)entry->known_mac_size, (unsigned long long)entry->known_device_time, entry->known_device_size);
        } else if (!entry->failed && entry->on_mac && entry->on_device) {
            fprintf(file, "%s\t%lld\t%llu\t%llu\t%u\n", entry->path, (long long)entry->mac_time,
                    (unsigned long long)entry->mac_size, (unsigned long long)entry->device_time, entry->device_size);
        }
    }
    if (fclose(file) == 0) rename(partial, manifest_path);
}

static void make_local_directories(const char *path) {
    char directory[PATH_SIZE * 2];
    snprintf(directory, sizeof directory, "%s", path);
    for (char *p = directory + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        mkdir(directory, 0755);
        *p = '/';
    }
}

static void make_remote_directories(sync_t *sync, const char *relative) {
    char partial[PATH_SIZE];
    snprintf(partial, sizeof partial, "%s", relative);
    for (char *p = partial; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        char remote[PATH_SIZE * 2];
        rapi_file_t info;
        remote_path(sync, partial, remote, sizeof remote);
        if (!rapi_stat(sync->rapi, remote, &info)) rapi_make_directory(sync->rapi, remote);
        *p = '/';
    }
}

#ifdef __APPLE__
static bool move_to_trash(const char *path) {
    const char *home = getenv("HOME");
    const char *leaf = strrchr(path, '/');
    if (!home || !leaf) return false;
    char trash[PATH_SIZE * 2];
    snprintf(trash, sizeof trash, "%s/.Trash", home);
    struct stat info;
    if (stat(trash, &info) != 0 || !S_ISDIR(info.st_mode)) return false;
    for (int copy = 0; copy < 100; copy++) {
        char target[PATH_SIZE * 3];
        if (copy) snprintf(target, sizeof target, "%s/%s %d", trash, leaf + 1, copy);
        else snprintf(target, sizeof target, "%s/%s", trash, leaf + 1);
        if (access(target, F_OK) != 0) return rename(path, target) == 0;
    }
    return false;
}
#else
static void percent_encode(const char *text, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t length = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && length + 4 < size; p++) {
        bool plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || strchr("/-_.~", *p);
        if (plain) {
            out[length++] = (char)*p;
        } else {
            out[length++] = '%';
            out[length++] = hex[*p >> 4];
            out[length++] = hex[*p & 0x0F];
        }
    }
    out[length] = 0;
}

static bool make_trash_folder(const char *path) {
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool write_trash_info(int descriptor, const char *path) {
    char encoded[PATH_SIZE * 6], deleted[32];
    time_t now = time(NULL);
    struct tm local;
    percent_encode(path, encoded, sizeof encoded);
    strftime(deleted, sizeof deleted, "%Y-%m-%dT%H:%M:%S", localtime_r(&now, &local));
    FILE *file = fdopen(descriptor, "w");
    if (!file) {
        close(descriptor);
        return false;
    }
    fprintf(file, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", encoded, deleted);
    return fclose(file) == 0;
}

static bool fits(int length, size_t size) {
    return length >= 0 && (size_t)length < size;
}

static bool move_to_trash(const char *path) {
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    const char *leaf = strrchr(path, '/');
    if (!leaf || path[0] != '/') return false;
    char trash[PATH_SIZE * 2], files[PATH_SIZE * 2], info[PATH_SIZE * 2];
    int trash_length;
    if (data_home && data_home[0] == '/') trash_length = snprintf(trash, sizeof trash, "%s/Trash", data_home);
    else if (home) trash_length = snprintf(trash, sizeof trash, "%s/.local/share/Trash", home);
    else return false;
    if (!fits(trash_length, sizeof trash) || !fits(snprintf(files, sizeof files, "%s/files", trash), sizeof files) ||
        !fits(snprintf(info, sizeof info, "%s/info", trash), sizeof info)) return false;
    if (!make_trash_folder(trash) || !make_trash_folder(files) || !make_trash_folder(info)) return false;
    for (int copy = 0; copy < 100; copy++) {
        char name[PATH_SIZE], target[PATH_SIZE * 3], info_path[PATH_SIZE * 3];
        int name_length = copy ? snprintf(name, sizeof name, "%s %d", leaf + 1, copy) : snprintf(name, sizeof name, "%s", leaf + 1);
        if (!fits(name_length, sizeof name) || !fits(snprintf(target, sizeof target, "%s/%s", files, name), sizeof target) ||
            !fits(snprintf(info_path, sizeof info_path, "%s/%s.trashinfo", info, name), sizeof info_path)) return false;
        int descriptor = open(info_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        if (access(target, F_OK) == 0) {
            close(descriptor);
            unlink(info_path);
            continue;
        }
        if (write_trash_info(descriptor, path) && rename(path, target) == 0) return true;
        unlink(info_path);
        return false;
    }
    return false;
}
#endif

static bool check_connection(sync_t *sync) {
    rapi_version_t version;
    if (!rapi_version(sync->rapi, &version)) sync->disconnected = true;
    return !sync->disconnected;
}

static bool upload(sync_t *sync, entry_t *entry) {
    char local[PATH_SIZE * 2], remote[PATH_SIZE * 2];
    local_path(sync, entry->path, local, sizeof local);
    remote_path(sync, entry->path, remote, sizeof remote);
    if (!valid_on_device(entry->path)) {
        sync_log(sync, "skipped %s: name not allowed on the " RAPI_DEVICE, entry->path);
        sync->result->skipped++;
        return false;
    }
    if (entry->mac_size + FREE_MARGIN > sync->free_space) {
        sync_log(sync, "skipped %s: not enough storage on the " RAPI_DEVICE, entry->path);
        sync->result->skipped++;
        return false;
    }
    make_remote_directories(sync, entry->path);
    rapi_file_t info;
    if (!rapi_upload(sync->rapi, local, remote, NULL, NULL) || !rapi_stat(sync->rapi, remote, &info)) {
        sync_log(sync, "can't copy %s to the " RAPI_DEVICE ": %s", entry->path, rapi_error(sync->rapi));
        check_connection(sync);
        return false;
    }
    entry->on_device = true;
    entry->device_time = info.write_time;
    entry->device_size = info.size;
    sync->free_space -= entry->mac_size < sync->free_space ? entry->mac_size : sync->free_space;
    sync->result->uploaded++;
    sync_log(sync, "copied %s to the " RAPI_DEVICE, entry->path);
    return true;
}

static bool download_as(sync_t *sync, entry_t *entry, const char *relative) {
    char local[PATH_SIZE * 2], remote[PATH_SIZE * 2];
    local_path(sync, relative, local, sizeof local);
    remote_path(sync, entry->path, remote, sizeof remote);
    make_local_directories(local);
    if (!rapi_download(sync->rapi, remote, local, NULL, NULL)) {
        sync_log(sync, "can't copy %s from the " RAPI_DEVICE ": %s", entry->path, rapi_error(sync->rapi));
        check_connection(sync);
        return false;
    }
    sync->result->downloaded++;
    sync_log(sync, "copied %s from the " RAPI_DEVICE, relative);
    return true;
}

static bool download(sync_t *sync, entry_t *entry) {
    if (!download_as(sync, entry, entry->path)) return false;
    char local[PATH_SIZE * 2];
    struct stat info;
    local_path(sync, entry->path, local, sizeof local);
    if (stat(local, &info) != 0) return false;
    entry->on_mac = true;
    entry->mac_time = (int64_t)info.st_mtime;
    entry->mac_size = (uint64_t)info.st_size;
    return true;
}

static bool resolve_conflict(sync_t *sync, entries_t *entries, size_t index) {
    char copy[PATH_SIZE];
    const char *path = entries->items[index].path;
    const char *leaf = strrchr(path, '/');
    const char *dot = strrchr(leaf ? leaf : path, '.');
    if (dot && dot != (leaf ? leaf + 1 : path)) snprintf(copy, sizeof copy, "%.*s (" RAPI_DEVICE ")%s", (int)(dot - path), path, dot);
    else snprintf(copy, sizeof copy, "%s (" RAPI_DEVICE ")", path);
    if (!download_as(sync, &entries->items[index], copy)) return false;
    sync->result->conflicts++;
    sync_log(sync, "%s changed on both: kept the " RAPI_DEVICE "'s copy as %s", path, copy);
    char local[PATH_SIZE * 2];
    struct stat info;
    local_path(sync, copy, local, sizeof local);
    entry_t *added = find_or_add(entries, copy);
    if (added && stat(local, &info) == 0) {
        added->on_mac = true;
        added->mac_time = (int64_t)info.st_mtime;
        added->mac_size = (uint64_t)info.st_size;
    }
    return upload(sync, &entries->items[index]);
}

static bool apply(sync_t *sync, entries_t *entries, size_t index) {
    entry_t *entry = &entries->items[index];
    bool mac_changed = entry->on_mac && (!entry->in_manifest || entry->mac_time != entry->known_mac_time || entry->mac_size != entry->known_mac_size);
    bool device_changed = entry->on_device && (!entry->in_manifest || entry->device_time != entry->known_device_time || entry->device_size != entry->known_device_size);
    if (entry->on_mac && entry->on_device) {
        if (!entry->in_manifest && entry->mac_size == entry->device_size) return true;
        if (!entry->in_manifest || (mac_changed && device_changed)) return resolve_conflict(sync, entries, index);
        if (mac_changed) return upload(sync, entry);
        if (device_changed) return download(sync, entry);
        return true;
    }
    if (entry->on_mac) {
        if (!entry->in_manifest || mac_changed) return upload(sync, entry);
        char local[PATH_SIZE * 2];
        local_path(sync, entry->path, local, sizeof local);
        if (!move_to_trash(local)) {
            sync_log(sync, "%s was deleted on the " RAPI_DEVICE "; couldn't move the Mac copy to the Trash", entry->path);
            return false;
        }
        entry->on_mac = false;
        sync->result->deleted_on_mac++;
        sync_log(sync, "%s was deleted on the " RAPI_DEVICE ": moved the Mac copy to the Trash", entry->path);
        return true;
    }
    if (entry->on_device) {
        if (!entry->in_manifest || device_changed) return download(sync, entry);
        char remote[PATH_SIZE * 2];
        remote_path(sync, entry->path, remote, sizeof remote);
        if (!rapi_delete(sync->rapi, remote)) {
            sync_log(sync, "can't delete %s on the " RAPI_DEVICE ": %s", entry->path, rapi_error(sync->rapi));
            check_connection(sync);
            return false;
        }
        entry->on_device = false;
        sync->result->deleted_on_device++;
        sync_log(sync, "%s was deleted on the host: deleted it on the " RAPI_DEVICE, entry->path);
        return true;
    }
    return true;
}

bool rapi_sync_run(rapi_t *rapi, const char *folder, const char *remote_root, const char *manifest_path,
                   rapi_sync_log_fn log, void *context, rapi_sync_result_t *result) {
    rapi_sync_result_t counts = { 0 };
    sync_t sync = { rapi, folder, remote_root, log, context, &counts, 0, false };
    entries_t entries = { 0 };
    rapi_store_t store;
    sync.free_space = rapi_store(rapi, &store) ? store.free_size : 0;
    load_manifest(&sync, &entries, manifest_path);
    scan_mac(&sync, &entries, "");
    if (!scan_device(&sync, &entries, "", 0)) {
        free(entries.items);
        return false;
    }
    size_t applied = 0;
    for (; applied < entries.count && !sync.disconnected; applied++) {
        if (!apply(&sync, &entries, applied)) entries.items[applied].failed = true;
    }
    for (size_t i = applied; i < entries.count; i++) entries.items[i].failed = true;
    save_manifest(&sync, &entries, manifest_path);
    free(entries.items);
    if (result) *result = counts;
    return !sync.disconnected;
}

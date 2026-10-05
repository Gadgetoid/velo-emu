#include "rapi/rapi.h"
#include "rapi/rapi_load.h"
#include "rapi/rapi_setup.h"
#include "rapi/rapi_sync.h"
#include "util/options.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define REMOTE_HOME "\\My Documents"

static const char *usage =
    "usage: velo-rapi [--socket=PATH | --connect=HOST:PORT] [--timeout=SECONDS] COMMAND [ARGUMENTS]\n"
    "Talks to a running Velo over RAPI (the emulator with Network (PPP) connected, or headless --net --rapi).\n"
    "\n"
    "  info                     OS version and storage\n"
    "  ls [PATH]                list a folder (default \\My Documents)\n"
    "  get PATH [LOCAL]         copy a file from the Velo\n"
    "  put LOCAL [PATH]         copy a file to the Velo\n"
    "  rm PATH                  delete a file\n"
    "  mkdir PATH | rmdir PATH  create or remove a folder\n"
    "  mv FROM TO               move or rename\n"
    "  run PROGRAM [ARGUMENTS]  start a program\n"
    "  sync FOLDER              sync a local folder with \\My Documents\n"
    "  load SCRIPT [DEST]       run an H/PC Explorer .load install script (DEST from its Install.inf, or \\Program Files\\Accessories)\n"
    "  proxy on|off             point Pocket IE at the emulator's web proxy\n"
    "  baud 19200|38400|57600|115200  desktop connection speed, from the next connection\n"
    "  reg ls|dump KEY          list a registry key, or everything under it\n"
    "  reg get KEY NAME         read a value\n"
    "  reg set KEY NAME dword|string VALUE\n"
    "\n"
    "Velo paths are relative to \\My Documents unless they start with / or \\; / and \\ both separate folders.\n"
    "--socket=PATH picks another RAPI socket (default rapi.sock in the data folder); --connect=HOST:PORT uses an emulator's RAPI over the Network instead; --timeout=SECONDS is how long to wait for the Velo once connected (default 30); --help and --version as usual.\n";

static void velo_path(const char *path, char *out, size_t size) {
    if (path[0] == '/' || path[0] == '\\') snprintf(out, size, "%s", path);
    else if (*path) snprintf(out, size, "%s\\%s", REMOTE_HOME, path);
    else snprintf(out, size, "%s", REMOTE_HOME);
    for (char *p = out; *p; p++) {
        if (*p == '/') *p = '\\';
    }
    size_t length = strlen(out);
    while (length > 1 && out[length - 1] == '\\') out[--length] = 0;
}

static const char *leaf_of(const char *path) {
    const char *leaf = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') leaf = p + 1;
    }
    return leaf;
}

static void show_progress(void *context, uint64_t done, uint64_t total) {
    if (!isatty(STDERR_FILENO)) return;
    fprintf(stderr, "\r%s %llu%%", (const char *)context, total ? (unsigned long long)(done * 100 / total) : 100ull);
    if (done >= total) fputc('\n', stderr);
}

static void log_line(void *context, const char *message) {
    (void)context;
    printf("%s\n", message);
}

static int fail(rapi_t *rapi) {
    fprintf(stderr, "velo-rapi: %s\n", rapi_error(rapi));
    return 1;
}

static int list(rapi_t *rapi, const char *path) {
    char pattern[1100];
    velo_path(path, pattern, sizeof pattern - 2);
    rapi_file_t info;
    if (strpbrk(pattern, "*?") == NULL && (!rapi_stat(rapi, pattern, &info) || (info.attributes & RAPI_ATTRIBUTE_DIRECTORY))) {
        strcat(pattern, "\\*");
    }
    rapi_file_t *files;
    size_t count;
    if (!rapi_list(rapi, pattern, &files, &count)) return fail(rapi);
    for (size_t i = 0; i < count; i++) {
        if (files[i].attributes & RAPI_ATTRIBUTE_DIRECTORY) printf("%10s  %s\\\n", "", files[i].name);
        else printf("%10u  %s\n", files[i].size, files[i].name);
    }
    free(files);
    return 0;
}

static int put(rapi_t *rapi, const char *local, const char *path) {
    char remote[1100];
    velo_path(path ? path : "", remote, sizeof remote);
    rapi_file_t info;
    if (!path || (rapi_stat(rapi, remote, &info) && (info.attributes & RAPI_ATTRIBUTE_DIRECTORY))) {
        size_t length = strlen(remote);
        snprintf(remote + length, sizeof remote - length, "\\%s", leaf_of(local));
    }
    return rapi_upload(rapi, local, remote, show_progress, (void *)leaf_of(local)) ? 0 : fail(rapi);
}

static int get(rapi_t *rapi, const char *path, const char *local) {
    char remote[1100];
    velo_path(path, remote, sizeof remote);
    const char *target = local ? local : leaf_of(remote);
    return rapi_download(rapi, remote, target, show_progress, (void *)leaf_of(remote)) ? 0 : fail(rapi);
}

static bool open_key(rapi_t *rapi, const char *path, bool create, uint32_t *key) {
    static const struct { const char *name; uint32_t key; } roots[] = {
        { "HKCR", RAPI_HKEY_CLASSES_ROOT }, { "HKEY_CLASSES_ROOT", RAPI_HKEY_CLASSES_ROOT },
        { "HKCU", RAPI_HKEY_CURRENT_USER }, { "HKEY_CURRENT_USER", RAPI_HKEY_CURRENT_USER },
        { "HKLM", RAPI_HKEY_LOCAL_MACHINE }, { "HKEY_LOCAL_MACHINE", RAPI_HKEY_LOCAL_MACHINE },
        { "HKU", RAPI_HKEY_USERS }, { "HKEY_USERS", RAPI_HKEY_USERS },
    };
    size_t root_length = strcspn(path, "/\\");
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        if (strlen(roots[i].name) != root_length || strncasecmp(path, roots[i].name, root_length)) continue;
        char subkey[1100];
        snprintf(subkey, sizeof subkey, "%s", path[root_length] ? path + root_length + 1 : "");
        for (char *p = subkey; *p; p++) {
            if (*p == '/') *p = '\\';
        }
        if (!*subkey) {
            *key = roots[i].key;
            return true;
        }
        return rapi_reg_open(rapi, roots[i].key, subkey, create, key);
    }
    fprintf(stderr, "velo-rapi: key must start with HKCR, HKCU, HKLM or HKU: %s\n", path);
    return false;
}

static void format_value(uint32_t type, const uint8_t *data, uint32_t length, char *out, size_t size) {
    if (type == RAPI_REG_DWORD && length >= 4) {
        uint32_t value = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
        snprintf(out, size, "dword %u (0x%x)", value, value);
    } else if (type == RAPI_REG_SZ || type == RAPI_REG_MULTI_SZ) {
        char text[RAPI_REG_DATA_MAX];
        uint8_t copy[RAPI_REG_DATA_MAX];
        memcpy(copy, data, length);
        for (uint32_t i = 0; type == RAPI_REG_MULTI_SZ && i + 3 < length; i += 2) {
            if (!copy[i] && !copy[i + 1] && (copy[i + 2] || copy[i + 3])) copy[i] = '|';
        }
        rapi_reg_text(copy, length, text, sizeof text);
        snprintf(out, size, "%s \"%s\"", type == RAPI_REG_SZ ? "string" : "multi", text);
    } else {
        int used = snprintf(out, size, "type %u:", type);
        for (uint32_t i = 0; i < length && used + 4 < (int)size; i++) used += snprintf(out + used, size - (size_t)used, " %02x", data[i]);
    }
}

static bool list_key(rapi_t *rapi, uint32_t key, const char *path, bool recursive) {
    for (uint32_t index = 0;; index++) {
        char name[RAPI_NAME_MAX], value[RAPI_REG_DATA_MAX * 3 + 32];
        uint8_t data[RAPI_REG_DATA_MAX];
        uint32_t type, length;
        bool found;
        if (!rapi_reg_value(rapi, key, index, name, sizeof name, &type, data, &length, &found)) return false;
        if (!found) break;
        format_value(type, data, length, value, sizeof value);
        if (recursive) printf("%s\\%s = %s\n", path, *name ? name : "@", value);
        else printf("%s = %s\n", *name ? name : "@", value);
    }
    for (uint32_t index = 0;; index++) {
        char name[RAPI_NAME_MAX];
        bool found;
        if (!rapi_reg_subkey(rapi, key, index, name, sizeof name, &found)) return false;
        if (!found) break;
        if (!recursive) {
            printf("%s\\\n", name);
            continue;
        }
        char child_path[2048];
        snprintf(child_path, sizeof child_path, "%s\\%s", path, name);
        uint32_t child;
        if (!rapi_reg_open(rapi, key, name, false, &child)) return false;
        bool listed = list_key(rapi, child, child_path, true);
        rapi_reg_close(rapi, child);
        if (!listed) return false;
    }
    return true;
}

static int registry(rapi_t *rapi, int count, char **args) {
    if (count < 2) return 2;
    const char *action = args[0];
    bool set = !strcmp(action, "set");
    if (!(!strcmp(action, "ls") && count == 2) && !(!strcmp(action, "dump") && count == 2) && !(!strcmp(action, "get") && count == 3) && !(set && count == 5)) return 2;
    uint32_t key;
    if (!open_key(rapi, args[1], set, &key)) return *rapi_error(rapi) ? fail(rapi) : 1;
    int status = 0;
    if (!strcmp(action, "ls") || !strcmp(action, "dump")) {
        if (!list_key(rapi, key, args[1], !strcmp(action, "dump"))) status = fail(rapi);
    } else if (!strcmp(action, "get")) {
        uint8_t data[RAPI_REG_DATA_MAX];
        uint32_t type, length;
        char value[RAPI_REG_DATA_MAX * 3 + 32];
        if (!rapi_reg_get(rapi, key, args[2], &type, data, sizeof data, &length)) status = fail(rapi);
        else {
            format_value(type, data, length, value, sizeof value);
            printf("%s\n", value);
        }
    } else if (!strcmp(args[3], "dword")) {
        uint32_t value = (uint32_t)strtoul(args[4], NULL, 0);
        uint8_t data[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
        if (!rapi_reg_set(rapi, key, args[2], RAPI_REG_DWORD, data, 4)) status = fail(rapi);
    } else if (!strcmp(args[3], "string")) {
        uint8_t data[RAPI_REG_DATA_MAX];
        uint32_t length = rapi_reg_encode(args[4], data, sizeof data);
        if (!rapi_reg_set(rapi, key, args[2], RAPI_REG_SZ, data, length)) status = fail(rapi);
    } else {
        status = 2;
    }
    if (key < RAPI_HKEY_CLASSES_ROOT) rapi_reg_close(rapi, key);
    return status;
}

static int sync_folder(rapi_t *rapi, const char *folder) {
    char manifest[1100], absolute[PATH_MAX];
    if (!realpath(folder, absolute)) {
        fprintf(stderr, "velo-rapi: no folder %s\n", folder);
        return 1;
    }
    rapi_data_path("sync-manifest.txt", manifest, sizeof manifest);
    rapi_sync_result_t result;
    if (!rapi_sync_run(rapi, absolute, REMOTE_HOME, manifest, log_line, NULL, &result)) return fail(rapi);
    printf("%u to the Velo, %u from the Velo, %u deleted on the Mac, %u deleted on the Velo, %u conflicts, %u skipped\n",
           result.uploaded, result.downloaded, result.deleted_on_mac, result.deleted_on_velo, result.conflicts, result.skipped);
    return 0;
}

int main(int argc, char **argv) {
    char socket_path[1024], connect_to[300] = "";
    rapi_data_path("rapi.sock", socket_path, sizeof socket_path);
    long timeout = 0;
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        const char *option = argv[first];
        if (!strcmp(option, "--help") || !strcmp(option, "-h")) {
            fputs(usage, stdout);
            return 0;
        }
        if (!strcmp(option, "--version")) {
            const char *slash = strrchr(argv[0], '/');
            printf("%s %s\n", slash ? slash + 1 : argv[0], options_version());
            return 0;
        }
        if (!strncmp(option, "--socket=", 9)) snprintf(socket_path, sizeof socket_path, "%s", option + 9);
        else if (!strcmp(option, "--socket") && first + 1 < argc) snprintf(socket_path, sizeof socket_path, "%s", argv[++first]);
        else if (!strncmp(option, "--connect=", 10) || (!strcmp(option, "--connect") && first + 1 < argc)) {
            const char *value = option[9] == '=' ? option + 10 : argv[++first];
            const char *colon = strrchr(value, ':');
            if (!colon || colon == value || !colon[1] || strlen(value) >= sizeof connect_to) {
                fprintf(stderr, "velo-rapi: --connect wants HOST:PORT, got %s\n", value);
                return 2;
            }
            snprintf(connect_to, sizeof connect_to, "%s", value);
        }
        else if (!strncmp(option, "--timeout=", 10) || (!strcmp(option, "--timeout") && first + 1 < argc)) {
            const char *value = option[9] == '=' ? option + 10 : argv[++first];
            if (!option_integer(value, 10, &timeout) || timeout <= 0 || timeout > 3600) {
                fprintf(stderr, "velo-rapi: --timeout wants SECONDS, got %s\n", value);
                return 2;
            }
        } else {
            fprintf(stderr, "velo-rapi: unknown option %s (see --help)\n", option);
            return 2;
        }
    }
    if (argc <= first) {
        fputs(usage, stderr);
        return 2;
    }
    const char *command = argv[first];
    int count = argc - first - 1;
    char **args = argv + first + 1;
    char error[1200];
    rapi_t *rapi;
    if (connect_to[0]) {
        char *colon = strrchr(connect_to, ':');
        *colon = 0;
        rapi = rapi_connect_tcp(connect_to, colon + 1, error, sizeof error);
    } else {
        rapi = rapi_connect(socket_path, error, sizeof error);
    }
    if (!rapi) {
        fprintf(stderr, "velo-rapi: %s\n", error);
        return 1;
    }
    if (timeout) rapi_set_timeout(rapi, (int)timeout);
    char path[1100], second[1100];
    int status = 2;
    if (!strcmp(command, "info") && count == 0) {
        rapi_version_t version;
        rapi_store_t store;
        if (!rapi_version(rapi, &version) || !rapi_store(rapi, &store)) status = fail(rapi);
        else {
            printf("Windows CE %u.%02u build %u\nstorage %u KB, %u KB free\n", version.major, version.minor, version.build,
                   store.store_size / 1024, store.free_size / 1024);
            status = 0;
        }
    }
    else if (!strcmp(command, "ls") && count <= 1) status = list(rapi, count ? args[0] : "");
    else if (!strcmp(command, "get") && (count == 1 || count == 2)) status = get(rapi, args[0], count == 2 ? args[1] : NULL);
    else if (!strcmp(command, "put") && (count == 1 || count == 2)) status = put(rapi, args[0], count == 2 ? args[1] : NULL);
    else if (!strcmp(command, "rm") && count == 1) { velo_path(args[0], path, sizeof path); status = rapi_delete(rapi, path) ? 0 : fail(rapi); }
    else if (!strcmp(command, "mkdir") && count == 1) { velo_path(args[0], path, sizeof path); status = rapi_make_directory(rapi, path) ? 0 : fail(rapi); }
    else if (!strcmp(command, "rmdir") && count == 1) { velo_path(args[0], path, sizeof path); status = rapi_remove_directory(rapi, path) ? 0 : fail(rapi); }
    else if (!strcmp(command, "mv") && count == 2) {
        velo_path(args[0], path, sizeof path);
        velo_path(args[1], second, sizeof second);
        status = rapi_move(rapi, path, second) ? 0 : fail(rapi);
    }
    else if (!strcmp(command, "run") && count >= 1) {
        char arguments[1100] = "";
        for (int i = 1; i < count; i++) {
            size_t length = strlen(arguments);
            snprintf(arguments + length, sizeof arguments - length, "%s%s", i > 1 ? " " : "", args[i]);
        }
        const char *program = args[0];
        if (program[0] == '/') { velo_path(program, path, sizeof path); program = path; }
        status = rapi_run(rapi, program, arguments) ? 0 : fail(rapi);
    }
    else if (!strcmp(command, "sync") && count == 1) status = sync_folder(rapi, args[0]);
    else if (!strcmp(command, "reg")) status = registry(rapi, count, args);
    else if (!strcmp(command, "load") && (count == 1 || count == 2)) {
        char dest[1100] = "";
        if (count == 2) velo_path(args[1], dest, sizeof dest);
        status = rapi_load_run(rapi, args[0], dest, log_line, NULL) ? 0 : 1;
    }
    else if (!strcmp(command, "proxy") && count == 1 && (!strcmp(args[0], "on") || !strcmp(args[0], "off"))) {
        rapi_version_t version = { 0 };
        status = rapi_setup_proxy(rapi, !strcmp(args[0], "on")) ? 0 : fail(rapi);
        if (!status && rapi_version(rapi, &version) && version.major >= 2) printf("Pocket IE picks this up after a soft reset\n");
    }
    else if (!strcmp(command, "baud") && count == 1) {
        uint32_t baud = (uint32_t)strtoul(args[0], NULL, 10);
        if (baud == 19200 || baud == 38400 || baud == 57600 || baud == 115200) status = rapi_setup_connection(rapi, baud) ? 0 : fail(rapi);
    }
    if (status == 2) fputs(usage, stderr);
    rapi_disconnect(rapi);
    return status;
}

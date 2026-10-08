#include "rapi/rapi_load.h"
#include "rapi/rapi_project.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PATH_SIZE   1024
#define TOKENS_MAX  8
#define TOKEN_SIZE  1024
#define CPU_SUFFIX  RAPI_CPU_SUFFIX
#define CPU_EXT     ".mip"

typedef struct {
    rapi_t          *rapi;
    char source[PATH_SIZE];
    char dest[PATH_SIZE];
    char app_name[PATH_SIZE];
    rapi_load_log_fn log;
    void            *context;
} loader_t;

static void say(loader_t *loader, const char *format, ...) {
    if (!loader->log) return;
    char message[PATH_SIZE * 2];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    loader->log(loader->context, message);
}

static int tokenize(const char *line, char tokens[TOKENS_MAX][TOKEN_SIZE], size_t lengths[TOKENS_MAX]) {
    int count = 0;
    const char *p = line;
    while (*p && count < TOKENS_MAX) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p || (p[0] == '/' && p[1] == '/')) break;
        size_t length = 0;
        char *out = tokens[count];
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++) {
                char c = *p;
                if (c == '\\' && (p[1] == '"' || p[1] == '0' || p[1] == '\\' || p[1] == 'n')) {
                    p++;
                    c = *p == '0' ? '\0' : *p == 'n' ? '\n' : *p;
                }
                if (length + 1 < TOKEN_SIZE) out[length++] = c;
            }
            if (*p == '"') p++;
        } else {
            while (*p && !isspace((unsigned char)*p) && length + 1 < TOKEN_SIZE) out[length++] = *p++;
        }
        out[length] = 0;
        lengths[count++] = length;
    }
    return count;
}

static void expand_install_path(const loader_t *loader, char *token, size_t *length) {
    char expanded[TOKEN_SIZE];
    size_t used = 0;
    for (size_t i = 0; i < *length && used + 1 < sizeof expanded; i++) {
        if (token[i] == '%' && (token[i + 1] == 'P' || token[i + 1] == 'p')) {
            used += (size_t)snprintf(expanded + used, sizeof expanded - used, "%s", loader->dest);
            if (used >= sizeof expanded) used = sizeof expanded - 1;
            i++;
        } else {
            expanded[used++] = token[i];
        }
    }
    memcpy(token, expanded, used);
    token[used] = 0;
    *length = used;
}

static bool open_source_file(const loader_t *loader, const char *name, char *out, size_t size) {
    DIR *dir = opendir(loader->source);
    if (!dir) return false;
    struct dirent *item;
    bool found = false;
    while ((item = readdir(dir))) {
        if (strcasecmp(item->d_name, name)) continue;
        snprintf(out, size, "%s/%s", loader->source, item->d_name);
        found = true;
        break;
    }
    closedir(dir);
    return found;
}

static void read_install_inf(loader_t *loader, char *install_dir, size_t size) {
    char path[PATH_SIZE * 2];
    if (!open_source_file(loader, "Install.inf", path, sizeof path)) return;
    FILE *file = fopen(path, "r");
    if (!file) return;
    char line[TOKEN_SIZE];
    while (fgets(line, sizeof line, file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncasecmp(line, "AppName=", 8)) snprintf(loader->app_name, sizeof loader->app_name, "%s", line + 8);
        else if (!strncasecmp(line, "InstallDir=", 11)) snprintf(install_dir, size, "%s", line + 11);
    }
    fclose(file);
}

static void device_path(const loader_t *loader, const char *directory, const char *name, char *out, size_t size) {
    const char *base = !strcmp(directory, ".") ? loader->dest : directory;
    size_t length = strlen(base);
    if (!name || !*name) snprintf(out, size, "%s", base);
    else if (length && base[length - 1] == '\\') snprintf(out, size, "%s%s", base, name);
    else snprintf(out, size, "%s\\%s", base, name);
}

static bool find_source(const loader_t *loader, const char *name, char *out, size_t size) {
    char wanted[3][PATH_SIZE];
    snprintf(wanted[0], sizeof wanted[0], "%s%s", name, CPU_SUFFIX);
    const char *dot = strrchr(name, '.');
    snprintf(wanted[1], sizeof wanted[1], "%.*s%s", dot ? (int)(dot - name) : (int)strlen(name), name, CPU_EXT);
    snprintf(wanted[2], sizeof wanted[2], "%s", name);
    for (int choice = 0; choice < 3; choice++) {
        if (open_source_file(loader, wanted[choice], out, size)) return true;
    }
    return false;
}

static void make_directories(loader_t *loader, const char *path) {
    if (!*path) return;
    char partial[PATH_SIZE];
    snprintf(partial, sizeof partial, "%s", path);
    for (char *p = partial + 1; ; p++) {
        if (*p != '\\' && *p) continue;
        char saved = *p;
        *p = 0;
        rapi_file_t info;
        if (!rapi_stat(loader->rapi, partial, &info)) rapi_make_directory(loader->rapi, partial);
        if (!saved) break;
        *p = saved;
    }
}

static bool copy_file(loader_t *loader, const char *directory, const char *name) {
    char local[PATH_SIZE * 2], folder[PATH_SIZE], remote[PATH_SIZE * 2];
    if (!find_source(loader, name, local, sizeof local)) {
        say(loader, "missing %s in %s", name, loader->source);
        return false;
    }
    device_path(loader, directory, NULL, folder, sizeof folder);
    make_directories(loader, folder);
    device_path(loader, directory, name, remote, sizeof remote);
    if (!rapi_upload(loader->rapi, local, remote, NULL, NULL)) {
        say(loader, "can't copy %s: %s", name, rapi_error(loader->rapi));
        return false;
    }
    say(loader, "copied %s", remote);
    return true;
}

static bool create_shortcut(loader_t *loader, const char *directory, const char *name, const char *target_directory, const char *target) {
    char folder[PATH_SIZE], link[PATH_SIZE * 2], path[PATH_SIZE * 2], content[PATH_SIZE * 2];
    device_path(loader, directory, NULL, folder, sizeof folder);
    device_path(loader, directory, name, link, sizeof link);
    if (!strcmp(target, "\"\"")) device_path(loader, target_directory, NULL, path, sizeof path);
    else device_path(loader, target_directory, target, path, sizeof path);
    if (strchr(path, '"')) snprintf(content, sizeof content, "\"%s", path);
    else if (strchr(path, ' ')) snprintf(content, sizeof content, "\"%s\"", path);
    else snprintf(content, sizeof content, "%s", path);
    char file[PATH_SIZE * 2 + 16];
    int length = snprintf(file, sizeof file, "%zu#%s", strlen(content), content);
    make_directories(loader, folder);
    if (!rapi_put(loader->rapi, link, file, (size_t)length)) {
        say(loader, "can't create %s: %s", link, rapi_error(loader->rapi));
        return false;
    }
    say(loader, "shortcut %s -> %s", link, content);
    return true;
}

static uint32_t root_key(const char *name) {
    if (!strcasecmp(name, "HKEY_CLASSES_ROOT")) return RAPI_HKEY_CLASSES_ROOT;
    if (!strcasecmp(name, "HKEY_CURRENT_USER")) return RAPI_HKEY_CURRENT_USER;
    if (!strcasecmp(name, "HKEY_USERS")) return RAPI_HKEY_USERS;
    return RAPI_HKEY_LOCAL_MACHINE;
}

static bool open_key(loader_t *loader, const char *root, const char *subkey, uint32_t *key, char *label, size_t size) {
    char app_key[PATH_SIZE + 16];
    if (!strcmp(subkey, "~")) {
        snprintf(app_key, sizeof app_key, "Software\\Apps\\%s", loader->app_name);
        subkey = app_key;
    }
    const char *root_name = !strcmp(root, "~") ? "HKEY_LOCAL_MACHINE" : root;
    snprintf(label, size, "%s\\%s", root_name, subkey);
    if (rapi_reg_open(loader->rapi, root_key(root_name), subkey, true, key)) return true;
    say(loader, "%s", rapi_error(loader->rapi));
    return false;
}

static const char *value_name(const char *name) {
    return !strcasecmp(name, "Default") ? "" : name;
}

static bool create_key(loader_t *loader, const char *root, const char *subkey) {
    uint32_t key;
    char label[PATH_SIZE * 2];
    if (!open_key(loader, root, subkey, &key, label, sizeof label)) return false;
    rapi_reg_close(loader->rapi, key);
    say(loader, "registry %s", label);
    return true;
}

static bool set_value(loader_t *loader, const char *root, const char *subkey, const char *name, uint32_t type, const uint8_t *data, uint32_t length) {
    uint32_t key;
    char label[PATH_SIZE * 2];
    if (!open_key(loader, root, subkey, &key, label, sizeof label)) return false;
    bool written = rapi_reg_set(loader->rapi, key, value_name(name), type, data, length);
    rapi_reg_close(loader->rapi, key);
    if (!written) say(loader, "%s", rapi_error(loader->rapi));
    else say(loader, "registry %s\\%s", label, name);
    return written;
}

static bool set_int(loader_t *loader, const char *root, const char *subkey, const char *name, const char *value) {
    uint32_t number = (uint32_t)strtoul(value, NULL, 0);
    uint8_t data[4] = { number & 0xFF, (number >> 8) & 0xFF, (number >> 16) & 0xFF, number >> 24 };
    return set_value(loader, root, subkey, name, RAPI_REG_DWORD, data, sizeof data);
}

static bool set_string(loader_t *loader, const char *root, const char *subkey, const char *name, const char *value, size_t value_length, bool multi) {
    uint8_t data[RAPI_REG_DATA_MAX];
    uint32_t length = 0;
    for (size_t start = 0; start <= value_length && length + 4 < sizeof data;) {
        size_t end = start;
        while (end < value_length && value[end]) end++;
        char part[TOKEN_SIZE];
        snprintf(part, sizeof part, "%.*s", (int)(end - start), value + start);
        length += rapi_reg_encode(part, data + length, sizeof data - length);
        if (!multi || end >= value_length) break;
        start = end + 1;
    }
    if (multi && length + 2 <= sizeof data) {
        data[length++] = 0;
        data[length++] = 0;
    }
    return set_value(loader, root, subkey, name, multi ? RAPI_REG_MULTI_SZ : RAPI_REG_SZ, data, length);
}

bool rapi_load_run(rapi_t *rapi, const char *script, const char *dest, rapi_load_log_fn log, void *context) {
    loader_t loader = { .rapi = rapi, .log = log, .context = context };
    const char *slash = strrchr(script, '/');
    if (slash) snprintf(loader.source, sizeof loader.source, "%.*s", (int)(slash - script), script);
    else snprintf(loader.source, sizeof loader.source, ".");
    const char *leaf = slash ? slash + 1 : script;
    const char *extension = strrchr(leaf, '.');
    snprintf(loader.app_name, sizeof loader.app_name, "%.*s", extension ? (int)(extension - leaf) : (int)strlen(leaf), leaf);
    char install_dir[PATH_SIZE] = RAPI_LOAD_DEFAULT_DEST;
    read_install_inf(&loader, install_dir, sizeof install_dir);
    snprintf(loader.dest, sizeof loader.dest, "%s", dest && *dest ? dest : install_dir);
    FILE *file = fopen(script, "r");
    if (!file) {
        say(&loader, "can't read %s", script);
        return false;
    }
    char tokens[TOKENS_MAX][TOKEN_SIZE];
    size_t lengths[TOKENS_MAX];
    char line[TOKEN_SIZE * 2];
    bool success = true;
    int number = 0;
    while (fgets(line, sizeof line, file)) {
        number++;
        line[strcspn(line, "\r\n")] = 0;
        int count = tokenize(line, tokens, lengths);
        if (!count) continue;
        for (int i = 1; i < count; i++) expand_install_path(&loader, tokens[i], &lengths[i]);
        const char *command = tokens[0];
        bool ok = true;
        if (!strcasecmp(command, "exit")) break;
        else if ((!strcasecmp(command, "copy") || !strcasecmp(command, "copyshared")) && count == 4) ok = copy_file(&loader, tokens[2], tokens[3]);
        else if (!strcasecmp(command, "mkdir") && count == 2) { make_directories(&loader, tokens[1]); say(&loader, "folder %s", tokens[1]); }
        else if (!strcasecmp(command, "createShortcut") && count == 5) ok = create_shortcut(&loader, tokens[1], tokens[2], tokens[3], tokens[4]);
        else if (!strcasecmp(command, "regKeyCreate") && count == 4) ok = create_key(&loader, tokens[1], tokens[2]);
        else if (!strcasecmp(command, "regInt") && count == 6) ok = set_int(&loader, tokens[1], tokens[2], tokens[4], tokens[5]);
        else if (!strcasecmp(command, "execOnUnload") && count == 2) say(&loader, "skipped %s, run on uninstall", tokens[1]);
        else if (!strcasecmp(command, "regString") && count == 6) ok = set_string(&loader, tokens[1], tokens[2], tokens[4], tokens[5], lengths[5], false);
        else if (!strcasecmp(command, "regStringMulti") && count == 6) ok = set_string(&loader, tokens[1], tokens[2], tokens[4], tokens[5], lengths[5], true);
        else if (!strcasecmp(command, "remoteExec") && count >= 2) {
            char program[PATH_SIZE];
            if (tokens[1][0] == '\\') snprintf(program, sizeof program, "%s", tokens[1]);
            else device_path(&loader, ".", tokens[1], program, sizeof program);
            ok = rapi_run(rapi, program, count > 2 ? tokens[2] : NULL);
            say(&loader, ok ? "started %s" : "can't start %s", program);
        }
        else {
            say(&loader, "%s line %d: unknown command %s", script, number, command);
            ok = false;
        }
        if (!ok) success = false;
    }
    fclose(file);
    return success;
}

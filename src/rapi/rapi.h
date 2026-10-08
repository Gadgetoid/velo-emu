#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAPI_ATTRIBUTE_DIRECTORY 0x10
#define RAPI_NAME_MAX            780
#define RAPI_FOLDER_DEPTH_MAX    32

#define RAPI_HKEY_CLASSES_ROOT   0x80000000u
#define RAPI_HKEY_CURRENT_USER   0x80000001u
#define RAPI_HKEY_LOCAL_MACHINE  0x80000002u
#define RAPI_HKEY_USERS          0x80000003u

#define RAPI_REG_SZ              1
#define RAPI_REG_BINARY          3
#define RAPI_REG_DWORD           4
#define RAPI_REG_MULTI_SZ        7
#define RAPI_REG_DATA_MAX        4096

typedef struct rapi rapi_t;

typedef struct {
    char name[RAPI_NAME_MAX];
    uint32_t attributes;
    uint32_t size;
    uint64_t write_time;
} rapi_file_t;

typedef struct {
    uint32_t major, minor, build, platform;
} rapi_version_t;

typedef struct {
    uint32_t store_size, free_size;
} rapi_store_t;

typedef void (*rapi_progress_fn)(void *context, uint64_t done, uint64_t total);

bool        rapi_data_path(const char *leaf, char *path, size_t size);
rapi_t     *rapi_connect(const char *socket_path, char *error, size_t error_size);
rapi_t     *rapi_connect_tcp(const char *host, const char *port, char *error, size_t error_size);
void        rapi_set_timeout(rapi_t *rapi, int seconds);
uint32_t    rapi_os_major(const rapi_t *rapi);
void        rapi_disconnect(rapi_t *rapi);
const char *rapi_error(const rapi_t *rapi);

bool rapi_version(rapi_t *rapi, rapi_version_t *version);
bool rapi_store(rapi_t *rapi, rapi_store_t *store);
bool rapi_list(rapi_t *rapi, const char *pattern, rapi_file_t **files, size_t *count);
bool rapi_stat(rapi_t *rapi, const char *path, rapi_file_t *file);
bool rapi_delete(rapi_t *rapi, const char *path);
bool rapi_make_directory(rapi_t *rapi, const char *path);
bool rapi_remove_directory(rapi_t *rapi, const char *path);
bool rapi_move(rapi_t *rapi, const char *from, const char *to);
bool rapi_run(rapi_t *rapi, const char *program, const char *arguments);
bool rapi_reg_open(rapi_t *rapi, uint32_t parent, const char *subkey, bool create, uint32_t *key);
bool rapi_reg_close(rapi_t *rapi, uint32_t key);
bool rapi_reg_subkey(rapi_t *rapi, uint32_t key, uint32_t index, char *name, size_t size, bool *found);
bool rapi_reg_value(rapi_t *rapi, uint32_t key, uint32_t index, char *name, size_t size, uint32_t *type, uint8_t *data, uint32_t *length, bool *found);
bool rapi_reg_get(rapi_t *rapi, uint32_t key, const char *name, uint32_t *type, uint8_t *data, uint32_t capacity, uint32_t *length);
bool rapi_reg_set(rapi_t *rapi, uint32_t key, const char *name, uint32_t type, const uint8_t *data, uint32_t length);
void rapi_reg_text(const uint8_t *data, uint32_t length, char *out, size_t size);
uint32_t rapi_reg_encode(const char *text, uint8_t *out, size_t size);
bool rapi_put(rapi_t *rapi, const char *remote, const void *data, size_t length);
bool rapi_upload(rapi_t *rapi, const char *local, const char *remote, rapi_progress_fn progress, void *context);
bool rapi_download(rapi_t *rapi, const char *remote, const char *local, rapi_progress_fn progress, void *context);

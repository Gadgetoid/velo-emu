#pragma once
#include "rapi/rapi.h"

typedef void (*rapi_sync_log_fn)(void *context, const char *message);

typedef struct {
    unsigned uploaded, downloaded, deleted_on_mac, deleted_on_device, conflicts, skipped;
} rapi_sync_result_t;

bool rapi_sync_run(rapi_t *rapi, const char *folder, const char *remote_root, const char *manifest_path,
                   rapi_sync_log_fn log, void *context, rapi_sync_result_t *result);

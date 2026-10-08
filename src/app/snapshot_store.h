#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "core/machine.h"

typedef struct {
    char data_folder[1024];
} snapshot_store_t;

void snapshot_store_init(snapshot_store_t *store, const char *data_folder);
void snapshot_store_folder(snapshot_store_t *store, char *path, size_t size);
void snapshot_store_default_name(snapshot_store_t *store, char *path, size_t size);
bool snapshot_store_backup_machine(snapshot_store_t *store, machine_t *machine, const char *state);
bool snapshot_store_backup_file(snapshot_store_t *store, const char *state);
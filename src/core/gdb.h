#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "core/machine.h"

typedef struct gdb gdb_t;

typedef void (*gdb_log_fn)(const char *message);

gdb_t *gdb_create(machine_t *machine, int port, bool network, gdb_log_fn log);
void   gdb_destroy(gdb_t *gdb);
bool   gdb_wait_for_client(gdb_t *gdb);
bool   gdb_connected(const gdb_t *gdb);
bool   gdb_set_process(gdb_t *gdb, const char *name);
void   gdb_debug_line(gdb_t *gdb, const char *line);
bool   gdb_run(gdb_t *gdb, uint64_t cycles);
void   gdb_service(gdb_t *gdb);
bool   gdb_halted(const gdb_t *gdb);
void   gdb_after_run(gdb_t *gdb);
void   gdb_set_machine(gdb_t *gdb, machine_t *machine);

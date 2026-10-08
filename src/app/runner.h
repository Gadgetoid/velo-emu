#pragma once

#include <stdbool.h>

#include "app/input.h"
#include "core/agent.h"
#include "core/gdb.h"

typedef struct app_runner app_runner_t;

app_runner_t *app_runner_create(machine_t *machine, input_queue_t *input, gdb_t *debugger, agent_t *agent);
void          app_runner_lock(app_runner_t *runner);
void          app_runner_unlock(app_runner_t *runner);
void          app_runner_set_machine_locked(app_runner_t *runner, machine_t *machine);
void          app_runner_set_debugger_locked(app_runner_t *runner, gdb_t *debugger);
void          app_runner_set_paused_locked(app_runner_t *runner, bool paused);
void          app_runner_destroy(app_runner_t *runner);
#include "app/runner.h"

#include <SDL3/SDL.h>

#include <stdlib.h>

#define RUN_HOLD_NS      (4 * SDL_NS_PER_MS)
#define RUN_SLICE_CYCLES (MACHINE_CLOCK_HZ / 1000)
#define RUN_MAX_BEHIND   (MACHINE_CLOCK_HZ / 10)

struct app_runner {
    SDL_Mutex    *lock;
    SDL_Thread   *thread;
    machine_t    *machine;
    input_queue_t *input;
    gdb_t        *debugger;
    agent_t      *agent;
    bool paused, stop, restart;
    SDL_AtomicInt waiting;
};

static int run_machine(void *context) {
    app_runner_t *runner = context;
    double owed = 0;
    uint64_t last = SDL_GetTicksNS();
    for (;;) {
        SDL_LockMutex(runner->lock);
        if (runner->stop) {
            SDL_UnlockMutex(runner->lock);
            return 0;
        }
        uint64_t now = SDL_GetTicksNS();
        if (runner->debugger) gdb_service(runner->debugger);
        if (runner->restart || runner->paused || (runner->debugger && gdb_halted(runner->debugger))) {
            owed = 0;
            runner->restart = false;
        } else {
            owed += (double)(now - last) * MACHINE_CLOCK_HZ / SDL_NS_PER_SECOND;
            if (owed > RUN_MAX_BEHIND) owed = RUN_MAX_BEHIND;
            uint64_t hold_until = now + RUN_HOLD_NS;
            while (owed >= RUN_SLICE_CYCLES && SDL_GetTicksNS() < hold_until && !SDL_GetAtomicInt(&runner->waiting)) {
                input_step(runner->input, runner->machine);
                machine_run(runner->machine, RUN_SLICE_CYCLES);
                owed -= RUN_SLICE_CYCLES;
                if (runner->agent) agent_poll(runner->agent, machine_mailbox(runner->machine));
                if (!runner->debugger) continue;
                gdb_after_run(runner->debugger);
                if (gdb_halted(runner->debugger)) break;
            }
        }
        last = now;
        bool caught_up = owed < RUN_SLICE_CYCLES;
        SDL_UnlockMutex(runner->lock);
        if (caught_up) SDL_DelayNS(SDL_NS_PER_MS / 2);
        while (SDL_GetAtomicInt(&runner->waiting)) SDL_DelayNS(SDL_NS_PER_MS / 10);
    }
}

app_runner_t *app_runner_create(machine_t *machine, input_queue_t *input, gdb_t *debugger, agent_t *agent) {
    app_runner_t *runner = calloc(1, sizeof *runner);
    if (!runner) return NULL;
    runner->lock = SDL_CreateMutex();
    if (!runner->lock) {
        free(runner);
        return NULL;
    }
    runner->machine = machine;
    runner->input = input;
    runner->debugger = debugger;
    runner->agent = agent;
    runner->restart = true;
    runner->thread = SDL_CreateThread(run_machine, "velo-machine", runner);
    if (!runner->thread) {
        SDL_DestroyMutex(runner->lock);
        free(runner);
        return NULL;
    }
    return runner;
}

void app_runner_lock(app_runner_t *runner) {
    SDL_SetAtomicInt(&runner->waiting, 1);
    SDL_LockMutex(runner->lock);
    SDL_SetAtomicInt(&runner->waiting, 0);
}

void app_runner_unlock(app_runner_t *runner) {
    SDL_UnlockMutex(runner->lock);
}

void app_runner_set_machine_locked(app_runner_t *runner, machine_t *machine) {
    runner->machine = machine;
    runner->restart = true;
}

void app_runner_set_paused_locked(app_runner_t *runner, bool paused) {
    runner->paused = paused;
}

void app_runner_destroy(app_runner_t *runner) {
    if (!runner) return;
    SDL_SetAtomicInt(&runner->waiting, 1);
    SDL_LockMutex(runner->lock);
    runner->stop = true;
    SDL_SetAtomicInt(&runner->waiting, 0);
    SDL_UnlockMutex(runner->lock);
    SDL_WaitThread(runner->thread, NULL);
    SDL_DestroyMutex(runner->lock);
    free(runner);
}
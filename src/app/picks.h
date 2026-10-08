#pragma once

#include <SDL3/SDL.h>

#include <stdbool.h>

#include "app/desktop.h"
#include "core/machine.h"

typedef enum { PICK_SEND = 1, PICK_FETCH, PICK_SHARED, PICK_SAVE_SNAPSHOT, PICK_LOAD_SNAPSHOT, PICK_CARD, PICK_DISK, PICK_NEW_DISK } pick_kind_t;

#define PICK_MAX 64

typedef struct {
    pick_kind_t kind;
    int count;
    char paths[PICK_MAX][1024];
    char export_uri[1024];
} picked_t;

typedef struct {
    char paths[PICK_MAX][1024];
    int count;
} dropped_t;

void        picks_init(void);
void        picks_done(void *userdata, const char *const *files, int filter);
picked_t   *picks_take(const SDL_Event *event);
bool        picks_create_blank_disk(const char *path);
const char *picks_handle_drop(dropped_t *dropped, machine_t *machine, desktop_t *desktop, bool online);
#ifdef __ANDROID__
picked_t   *picks_new_disk(void);
#endif

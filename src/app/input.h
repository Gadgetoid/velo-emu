#pragma once

#include <SDL3/SDL.h>

#include "core/machine.h"

#define INPUT_QUEUE 64

typedef enum { INPUT_PEN, INPUT_KEY } input_kind_t;

typedef struct {
    struct { uint64_t at; input_kind_t kind; bool down; int x, y; uint8_t scancode; } events[INPUT_QUEUE];
    int count;
    uint64_t last_at, seen;
} input_queue_t;

typedef struct {
    float vertical, horizontal;
    int pending;
    uint8_t scancode;
    bool pressed;
    uint64_t next_at;
} scroller_t;

bool input_find_scancode(key_layout_t layout, SDL_Keycode key, uint8_t *scancode);
void scroller_add(scroller_t *scroller, float vertical, float horizontal);
void scroller_step(scroller_t *scroller, machine_t *machine);
void input_add(input_queue_t *input, machine_t *machine, input_kind_t kind, bool down, int x, int y, uint8_t scancode);
void pen_move(input_queue_t *input, machine_t *machine, int x, int y);
void input_step(input_queue_t *input, machine_t *machine);
void input_clear(input_queue_t *input);
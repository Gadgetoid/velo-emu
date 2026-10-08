#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/machine.h"

#define PASTE_MAX 8192

typedef struct {
    char text[PASTE_MAX];
    size_t length, position;
    bool pressed, shifted;
    uint8_t scancode;
    uint64_t next_at;
    key_layout_t layout;
} typer_t;

size_t typer_start(typer_t *typer, key_layout_t layout, const char *utf8);
void   typer_step(typer_t *typer, machine_t *machine);

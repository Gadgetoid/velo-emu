#pragma once

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stddef.h>

bool host_confirm(SDL_Window *window, const char *title, const char *message, const char *action);
void host_open_path(const char *path);
void host_reveal_file(const char *path);
void host_reap_children(void);
void host_local_address(char *address, size_t size);

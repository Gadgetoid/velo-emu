#pragma once

#include <SDL3/SDL.h>

#include <stdbool.h>

void keystrip_install(SDL_Window *window);
void keystrip_insets(int *left, int *top, int *right, int *bottom);
bool keystrip_event(const SDL_Event *event, bool *open_menu);
void keystrip_release(void);
void keystrip_draw(SDL_Renderer *renderer);

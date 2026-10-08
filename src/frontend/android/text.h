#pragma once

#include <SDL3/SDL.h>

#include <stdbool.h>

float     text_width(SDL_Renderer *renderer, const char *text, float size);
void      text_draw(SDL_Renderer *renderer, float x, float y, const char *text, float size);
void      text_draw_centred(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float size);
float     text_fit(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float height);
float     text_display_scale(SDL_Window *window);
SDL_FRect text_inset(SDL_FRect rect, float gap);
bool      text_contains(const SDL_FRect *rect, float x, float y);

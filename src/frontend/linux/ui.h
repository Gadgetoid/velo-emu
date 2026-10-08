#pragma once

#include <SDL3/SDL.h>

#include <stdbool.h>

typedef struct { Uint8 r, g, b, a; } colour_t;

typedef struct {
    colour_t bar, bar_open, bar_border, text, menu, menu_border, highlight, highlight_text, disabled, shortcut, separator;
} palette_t;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *glyphs;
    int glyph_generation;
} canvas_t;

typedef enum { GLYPH_ELLIPSIS, GLYPH_CHECK, GLYPH_ARROW, GLYPH_EXTRA_COUNT } glyph_t;

void             ui_load_font(void);
void             ui_prepare(SDL_Window *window);
const palette_t *ui_palette(void);
float            ui_text_width(const char *text);
void             ui_fill(canvas_t *canvas, float x, float y, float width, float height, colour_t colour);
void             ui_text(canvas_t *canvas, float x, float top, float height, const char *text, colour_t colour);
void             ui_mark(canvas_t *canvas, glyph_t glyph, float centre_x, float top, float height, colour_t colour);
bool             ui_inside(float x, float y, float left, float top, float width, float height);
void             ui_release_glyphs(canvas_t *canvas);
void             ui_destroy_canvas(canvas_t *canvas);

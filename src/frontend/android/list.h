#pragma once

#include <SDL3/SDL.h>

#include <stdbool.h>
#include <stdint.h>

#define LIST_ROW_MAX   128
#define LIST_TAB_MAX   9
#define LIST_ROW_POINTS 48.0f
#define LIST_LABEL_HEIGHT 0.36f

typedef enum { ROW_ITEM, ROW_HEADING, ROW_SEPARATOR } row_kind_t;

typedef struct {
    row_kind_t kind;
    int tag;
    const char *title;
    bool checked, disabled;
} row_t;

typedef struct {
    const char *tabs[LIST_TAB_MAX];
    int tab_count, tab_selected;
    row_t rows[LIST_ROW_MAX];
    int row_count;
} list_t;

typedef struct {
    bool tapped;
    int tab, tag;
} tap_t;

typedef struct {
    float scroll, velocity;
    bool touching, dragging;
    float touch_x, touch_y, touch_scroll;
    float last_motion_y;
    uint64_t last_motion_ns, last_frame_ns;
} list_scroll_t;

void  list_add_row(list_t *list, row_kind_t kind, int tag, const char *title, bool checked, bool disabled);
void  list_reset(list_scroll_t *scroll);
tap_t list_event(list_scroll_t *scroll, const list_t *list, SDL_Window *window, const SDL_Event *event);
void  list_draw(list_scroll_t *scroll, const list_t *list, SDL_Window *window, SDL_Renderer *renderer);
tap_t list_modal_step(list_scroll_t *scroll, const list_t *list, SDL_Window *window, int cancel_tab);

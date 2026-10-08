#include "frontend/android/list.h"

#include <math.h>

#include "frontend/android/text.h"

#define ROW_POINTS        44.0f
#define GAP_POINTS        3.0f
#define LIST_HEAD_POINTS  32.0f
#define LIST_SEP_POINTS   12.0f
#define LIST_PAD_POINTS   16.0f
#define DRAG_POINTS       8.0f
#define FLING_DECAY       4.0f
#define FLING_STOP_POINTS 20.0f
#define HEADING_SIZE      0.8f
#define MARK_SIZE         0.6f
#define MODAL_WAIT_MS     16

void list_add_row(list_t *list, row_kind_t kind, int tag, const char *title, bool checked, bool disabled) {
    if (list->row_count < LIST_ROW_MAX) list->rows[list->row_count++] = (row_t){ kind, tag, title, checked, disabled };
}

void list_reset(list_scroll_t *scroll) {
    scroll->scroll = scroll->velocity = 0;
    scroll->touching = scroll->dragging = false;
    scroll->last_frame_ns = 0;
}

static float row_height(SDL_Window *window, row_kind_t kind) {
    float points = kind == ROW_ITEM ? LIST_ROW_POINTS : kind == ROW_HEADING ? LIST_HEAD_POINTS : LIST_SEP_POINTS;
    return floorf(points * text_display_scale(window));
}

static SDL_Rect list_area(SDL_Window *window) {
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(window, &safe);
    if (width > height) return (SDL_Rect){ safe.x, 0, safe.w, height };
    return (SDL_Rect){ 0, safe.y, width, safe.h };
}

static SDL_FRect tab_rect(SDL_Window *window, int index, int count) {
    SDL_Rect area = list_area(window);
    float scale = text_display_scale(window);
    float cell = (float)area.w / count;
    return text_inset((SDL_FRect){ area.x + index * cell, (float)area.y, cell, floorf(ROW_POINTS * scale) }, GAP_POINTS * scale);
}

static float list_top(SDL_Window *window) {
    float scale = text_display_scale(window);
    return (float)list_area(window).y + floorf(ROW_POINTS * scale) + floorf(GAP_POINTS * scale);
}

static void clamp_scroll(list_scroll_t *scroll, const list_t *list, SDL_Window *window) {
    float height = 0;
    for (int i = 0; i < list->row_count; i++) height += row_height(window, list->rows[i].kind);
    SDL_Rect area = list_area(window);
    float visible = (float)(area.y + area.h) - list_top(window);
    scroll->scroll = fminf(fmaxf(scroll->scroll, 0), fmaxf(0, height - visible));
}

static int row_at(const list_scroll_t *scroll, const list_t *list, SDL_Window *window, float y) {
    float top = list_top(window) - scroll->scroll;
    for (int i = 0; i < list->row_count; i++) {
        const row_t *row = &list->rows[i];
        float height = row_height(window, row->kind);
        if (y >= top && y < top + height) return row->kind == ROW_ITEM && !row->disabled ? row->tag : -1;
        top += height;
    }
    return -1;
}

static tap_t list_tap(const list_scroll_t *scroll, const list_t *list, SDL_Window *window, float x, float y) {
    tap_t tap = { true, -1, -1 };
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(window, i, list->tab_count);
        if (text_contains(&rect, x, y)) {
            tap.tab = i;
            return tap;
        }
    }
    if (y >= list_top(window)) tap.tag = row_at(scroll, list, window, y);
    return tap;
}

tap_t list_event(list_scroll_t *scroll, const list_t *list, SDL_Window *window, const SDL_Event *event) {
    tap_t tap = { false, -1, -1 };
    float threshold = DRAG_POINTS * text_display_scale(window);
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        scroll->touching = true;
        scroll->dragging = false;
        scroll->velocity = 0;
        scroll->touch_x = event->button.x;
        scroll->touch_y = event->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        if (!scroll->touching) break;
        if (!scroll->dragging && fabsf(event->motion.y - scroll->touch_y) > threshold && scroll->touch_y >= list_top(window)) {
            scroll->dragging = true;
            scroll->touch_y = scroll->last_motion_y = event->motion.y;
            scroll->touch_scroll = scroll->scroll;
            scroll->last_motion_ns = event->motion.timestamp;
        }
        if (!scroll->dragging) break;
        scroll->scroll = scroll->touch_scroll - (event->motion.y - scroll->touch_y);
        clamp_scroll(scroll, list, window);
        float seconds = (float)(event->motion.timestamp - scroll->last_motion_ns) / SDL_NS_PER_SECOND;
        if (seconds > 0) scroll->velocity = scroll->velocity * 0.5f + (scroll->last_motion_y - event->motion.y) / seconds * 0.5f;
        scroll->last_motion_y = event->motion.y;
        scroll->last_motion_ns = event->motion.timestamp;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (scroll->touching && !scroll->dragging) tap = list_tap(scroll, list, window, scroll->touch_x, scroll->touch_y);
        if (!scroll->dragging || (float)(event->button.timestamp - scroll->last_motion_ns) / SDL_NS_PER_SECOND > 0.1f) scroll->velocity = 0;
        scroll->touching = scroll->dragging = false;
        scroll->last_frame_ns = SDL_GetTicksNS();
        break;
    default:
        break;
    }
    return tap;
}

static void fling(list_scroll_t *scroll, const list_t *list, SDL_Window *window) {
    uint64_t now = SDL_GetTicksNS();
    float seconds = scroll->last_frame_ns ? (float)(now - scroll->last_frame_ns) / SDL_NS_PER_SECOND : 0;
    scroll->last_frame_ns = now;
    if (scroll->touching || scroll->velocity == 0 || seconds <= 0) return;
    float before = scroll->scroll;
    scroll->scroll += scroll->velocity * seconds;
    clamp_scroll(scroll, list, window);
    scroll->velocity *= expf(-FLING_DECAY * seconds);
    if (scroll->scroll == before || fabsf(scroll->velocity) < FLING_STOP_POINTS * text_display_scale(window)) scroll->velocity = 0;
}

void list_draw(list_scroll_t *scroll, const list_t *list, SDL_Window *window, SDL_Renderer *renderer) {
    fling(scroll, list, window);
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, (float)height });

    float scale = text_display_scale(window);
    float text_size = floorf(LIST_ROW_POINTS * scale * LIST_LABEL_HEIGHT);
    float heading_size = floorf(text_size * HEADING_SIZE);
    SDL_Rect area = list_area(window);
    float top = list_top(window);
    float pad = floorf(LIST_PAD_POINTS * scale);
    float mark = floorf(text_size * MARK_SIZE);
    float y = top - scroll->scroll;
    for (int i = 0; i < list->row_count; i++) {
        const row_t *row = &list->rows[i];
        float row_h = row_height(window, row->kind);
        if (row->kind == ROW_SEPARATOR) {
            SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
            SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + row_h / 2), area.w - 2 * pad, fmaxf(1, floorf(scale)) });
        } else if (row->kind == ROW_HEADING) {
            SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
            text_draw(renderer, area.x + pad, y + row_h - heading_size - floorf(4 * scale), row->title, heading_size);
        } else {
            if (row->checked) {
                SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
                SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + (row_h - mark) / 2), mark, mark });
            }
            if (row->disabled) SDL_SetRenderDrawColor(renderer, 0x66, 0x66, 0x66, 0xFF);
            else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
            text_draw(renderer, area.x + 2 * pad + mark, floorf(y + (row_h - text_size) / 2), row->title, text_size);
        }
        y += row_h;
    }

    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, top });
    float tab_size = text_size;
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(window, i, list->tab_count);
        tab_size = fminf(tab_size, text_fit(renderer, &rect, list->tabs[i], LIST_LABEL_HEIGHT * 1.2f));
    }
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(window, i, list->tab_count);
        bool lit = i == list->tab_selected;
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, &rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        text_draw_centred(renderer, &rect, list->tabs[i], tab_size);
    }
}

tap_t list_modal_step(list_scroll_t *scroll, const list_t *list, SDL_Window *window, int cancel_tab) {
    SDL_Renderer *renderer = SDL_GetRenderer(window);
    list_draw(scroll, list, window, renderer);
    SDL_RenderPresent(renderer);
    tap_t tap = { false, -1, -1 };
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, MODAL_WAIT_MS)) return tap;
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        SDL_PushEvent(&event);
        return (tap_t){ true, cancel_tab, -1 };
    case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_AC_BACK || event.key.key == SDLK_ESCAPE) return (tap_t){ true, cancel_tab, -1 };
        return tap;
    default:
        return list_event(scroll, list, window, &event);
    }
}

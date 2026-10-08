#include "frontend/android/keystrip.h"

#include <math.h>
#include <string.h>

#include "frontend/android/text.h"
#include "frontend/common/menu.h"
#include "frontend/common/menu_queue.h"
#include "frontend/common/menu_state.h"

#define ROW_POINTS    44.0f
#define COLUMN_POINTS 64.0f
#define TAB_POINTS    28.0f
#define GAP_POINTS    3.0f
#define LABEL_HEIGHT  0.36f

typedef enum { BUTTON_TOGGLE, BUTTON_MENU, BUTTON_KEYBOARD, BUTTON_KEY, BUTTON_MODIFIER, BUTTON_ITEM } button_kind_t;

typedef struct {
    const char   *label;
    button_kind_t kind;
    SDL_Keycode key;
    int item;
} button_t;

static const button_t BUTTONS[] = {
    { "Hide", BUTTON_TOGGLE, 0, 0 },
    { "Menu", BUTTON_MENU, 0, 0 },
    { "Esc", BUTTON_KEY, SDLK_ESCAPE, 0 },
    { "Tab", BUTTON_KEY, SDLK_TAB, 0 },
    { "Ctrl", BUTTON_MODIFIER, SDLK_LCTRL, 0 },
    { "Alt", BUTTON_MODIFIER, SDLK_LALT, 0 },
    { "Shift", BUTTON_MODIFIER, SDLK_LSHIFT, 0 },
    { "Kbd", BUTTON_KEYBOARD, 0, 0 },
    { "Power", BUTTON_ITEM, 0, MENU_POWER },
    { "Light", BUTTON_ITEM, 0, MENU_BACKLIGHT },
    { "^", BUTTON_KEY, SDLK_UP, 0 },
    { "<", BUTTON_KEY, SDLK_LEFT, 0 },
    { ">", BUTTON_KEY, SDLK_RIGHT, 0 },
    { "v", BUTTON_KEY, SDLK_DOWN, 0 },
    { "Enter", BUTTON_KEY, SDLK_RETURN, 0 },
};

#define BUTTON_COUNT (int)(sizeof BUTTONS / sizeof BUTTONS[0])
#define FIRST_GROUP  8

typedef struct {
    SDL_FRect buttons[BUTTON_COUNT];
    int left, top, right, bottom;
    float label_size;
} layout_t;

static SDL_Window *main_window;
static bool collapsed;
static int pressed = -1;
static bool latched[BUTTON_COUNT];

static const char *label_of(int index) {
    return BUTTONS[index].kind == BUTTON_TOGGLE && collapsed ? "Keys" : BUTTONS[index].label;
}

static SDL_FRect cell_in_group(int index, float along, float across, float depth, bool columns) {
    bool first = index < FIRST_GROUP;
    int position = first ? index : index - FIRST_GROUP;
    float size = along / (first ? FIRST_GROUP : BUTTON_COUNT - FIRST_GROUP);
    if (columns) return (SDL_FRect){ first ? 0 : across - depth, position * size, depth, size };
    return (SDL_FRect){ position * size, first ? 0 : depth, size, depth };
}

static layout_t layout(void) {
    layout_t result = { 0 };
    if (!main_window) return result;
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(main_window, &safe);
    float scale = text_display_scale(main_window);
    float gap = GAP_POINTS * scale, tab = floorf(TAB_POINTS * scale);
    bool landscape = width > height;
    int start = landscape ? safe.x : safe.y;
    int end = landscape ? width - safe.x - safe.w : height - safe.y - safe.h;
    float along = (float)(landscape ? height : width), across = (float)((landscape ? width : height) - start - end);
    float depth = floorf((landscape ? COLUMN_POINTS : ROW_POINTS) * scale);
    int leading = start + (int)(collapsed ? tab : depth);
    int trailing = end + (landscape && !collapsed ? (int)depth : 0);
    if (!landscape && !collapsed) leading = start + (int)(2 * depth);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (collapsed && BUTTONS[i].kind != BUTTON_TOGGLE) continue;
        SDL_FRect cell = cell_in_group(i, along, across, collapsed ? tab : depth, landscape);
        if (landscape) cell.x += start;
        else cell.y += start;
        result.buttons[i] = text_inset(cell, gap);
    }
    if (landscape) {
        result.left = leading;
        result.right = trailing;
    } else {
        result.top = leading;
        result.bottom = trailing;
    }
    SDL_Renderer *renderer = SDL_GetRenderer(main_window);
    result.label_size = INFINITY;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &result.buttons[i];
        if (rect->w > 0) result.label_size = fminf(result.label_size, text_fit(renderer, rect, label_of(i), LABEL_HEIGHT));
    }
    result.label_size = fmaxf(1, result.label_size);
    return result;
}

static int button_at(float x, float y) {
    layout_t current = layout();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (text_contains(&current.buttons[i], x, y)) return i;
    }
    return -1;
}

static void push_key(SDL_Keycode key, bool down) {
    SDL_Event event = { 0 };
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.windowID = main_window ? SDL_GetWindowID(main_window) : 0;
    event.key.key = key;
    event.key.down = down;
    SDL_PushEvent(&event);
}

void keystrip_release(void) {
    if (pressed < 0) return;
    push_key(BUTTONS[pressed].key, false);
    pressed = -1;
}

static void press(int index, bool *open_menu) {
    const button_t *button = &BUTTONS[index];
    switch (button->kind) {
    case BUTTON_TOGGLE:
        collapsed = !collapsed;
        break;
    case BUTTON_MENU:
        *open_menu = true;
        break;
    case BUTTON_KEYBOARD:
        if (SDL_ScreenKeyboardShown(main_window)) SDL_StopTextInput(main_window);
        else SDL_StartTextInput(main_window);
        break;
    case BUTTON_KEY:
        pressed = index;
        push_key(button->key, true);
        break;
    case BUTTON_MODIFIER:
        latched[index] = !latched[index];
        push_key(button->key, latched[index]);
        break;
    case BUTTON_ITEM:
        menu_queue_push(button->item);
        break;
    }
}

void keystrip_install(SDL_Window *window) {
    main_window = window;
}

void keystrip_insets(int *left, int *top, int *right, int *bottom) {
    layout_t current = layout();
    *left = current.left;
    *top = current.top;
    *right = current.right;
    *bottom = current.bottom;
}

bool keystrip_event(const SDL_Event *event, bool *open_menu) {
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int index = button_at(event->button.x, event->button.y);
        if (index < 0) return false;
        press(index, open_menu);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (pressed < 0) return false;
        keystrip_release();
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        return pressed >= 0;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        keystrip_release();
        memset(latched, 0, sizeof latched);
        return false;
    default:
        return false;
    }
}

void keystrip_draw(SDL_Renderer *renderer) {
    layout_t current = layout();
    bool keyboard = SDL_ScreenKeyboardShown(main_window);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &current.buttons[i];
        if (rect->w <= 0) continue;
        bool lit = i == pressed || latched[i] || (BUTTONS[i].kind == BUTTON_KEYBOARD && keyboard) || (BUTTONS[i].kind == BUTTON_ITEM && menu_state_checked(BUTTONS[i].item));
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        text_draw_centred(renderer, rect, label_of(i), current.label_size);
    }
}

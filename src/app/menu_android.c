#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "app/dialog.h"
#include "app/menu.h"

#define ROW_POINTS    44.0f
#define COLUMN_POINTS 64.0f
#define GAP_POINTS    3.0f
#define LABEL_WIDTH   0.85f
#define LABEL_HEIGHT  0.4f
#define MENU_QUEUE    16

typedef enum { BUTTON_KEYBOARD, BUTTON_KEY, BUTTON_MODIFIER, BUTTON_ITEM, BUTTON_NEXT_MACHINE } button_kind_t;

typedef struct {
    const char   *label;
    button_kind_t kind;
    SDL_Keycode   key;
    int           item;
} button_t;

static const button_t BUTTONS[] = {
    { "Swap", BUTTON_NEXT_MACHINE, 0, 0 },
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
#define GROUP_SIZE   ((BUTTON_COUNT + 1) / 2)

typedef struct {
    SDL_FRect buttons[BUTTON_COUNT];
    int       left, top, right, bottom;
    float     label_scale;
} layout_t;

static SDL_Window *main_window;
static int         pressed = -1;
static bool        latched[BUTTON_COUNT];
static bool        checked[MENU_COUNT];
static bool        hidden[MENU_COUNT];
static int         queue[MENU_QUEUE];
static int         queued;

static SDL_FRect inset(SDL_FRect rect, float gap) {
    return (SDL_FRect){ floorf(rect.x + gap / 2), floorf(rect.y + gap / 2), floorf(rect.w - gap), floorf(rect.h - gap) };
}

static layout_t layout(void) {
    layout_t result = { 0 };
    if (!main_window) return result;
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(main_window, &safe);
    float scale = SDL_GetWindowDisplayScale(main_window);
    if (scale <= 0) scale = 1;
    float gap = GAP_POINTS * scale;
    int safe_right = width - safe.x - safe.w, safe_bottom = height - safe.y - safe.h;
    if (width > height) {
        float column = floorf(COLUMN_POINTS * scale), cell = (float)safe.h / GROUP_SIZE;
        for (int i = 0; i < BUTTON_COUNT; i++) {
            float x = i < GROUP_SIZE ? (float)safe.x : (float)(safe.x + safe.w) - column;
            result.buttons[i] = inset((SDL_FRect){ x, safe.y + (i % GROUP_SIZE) * cell, column, cell }, gap);
        }
        result.left = safe.x + (int)column;
        result.right = safe_right + (int)column;
        result.top = safe.y;
        result.bottom = safe_bottom;
    } else {
        float row = floorf(ROW_POINTS * scale), cell = (float)safe.w / GROUP_SIZE;
        for (int i = 0; i < BUTTON_COUNT; i++) {
            result.buttons[i] = inset((SDL_FRect){ safe.x + (i % GROUP_SIZE) * cell, safe.y + (i / GROUP_SIZE) * row, cell, row }, gap);
        }
        result.left = safe.x;
        result.right = safe_right;
        result.top = safe.y + (int)(2 * row);
        result.bottom = safe_bottom;
    }
    result.label_scale = INFINITY;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        float length = (float)strlen(BUTTONS[i].label);
        SDL_FRect *rect = &result.buttons[i];
        float fit = fminf(rect->h * LABEL_HEIGHT, rect->w * LABEL_WIDTH / length) / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        result.label_scale = fminf(result.label_scale, fit);
    }
    result.label_scale = fmaxf(1, floorf(result.label_scale));
    return result;
}

static int button_at(float x, float y) {
    layout_t current = layout();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &current.buttons[i];
        if (x >= rect->x && x < rect->x + rect->w && y >= rect->y && y < rect->y + rect->h) return i;
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

static void enqueue(int item) {
    if (queued < MENU_QUEUE) queue[queued++] = item;
}

static void next_machine(void) {
    int current = -1;
    for (int i = MENU_MACHINE_FIRST; i <= MENU_MACHINE_LAST; i++) {
        if (!hidden[i] && checked[i]) current = i;
    }
    for (int step = 1; step <= MENU_MACHINE_LAST - MENU_MACHINE_FIRST; step++) {
        int candidate = MENU_MACHINE_FIRST + ((current < 0 ? 0 : current - MENU_MACHINE_FIRST) + step) % (MENU_MACHINE_LAST - MENU_MACHINE_FIRST + 1);
        if (!hidden[candidate]) {
            enqueue(candidate);
            return;
        }
    }
}

static void press(int index) {
    const button_t *button = &BUTTONS[index];
    switch (button->kind) {
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
        enqueue(button->item);
        break;
    case BUTTON_NEXT_MACHINE:
        next_machine();
        break;
    }
}

static void release(void) {
    if (pressed < 0) return;
    push_key(BUTTONS[pressed].key, false);
    pressed = -1;
}

void menu_install(SDL_Window *window) {
    main_window = window;
}

int menu_bar_height(void) {
    return layout().top;
}

void menu_insets(int *left, int *top, int *right, int *bottom) {
    layout_t current = layout();
    *left = current.left;
    *top = current.top;
    *right = current.right;
    *bottom = current.bottom;
}

bool menu_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int index = button_at(event->button.x, event->button.y);
        if (index < 0) return false;
        press(index);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (pressed < 0) return false;
        release();
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        return pressed >= 0;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release();
        memset(latched, 0, sizeof latched);
        return false;
    default:
        return false;
    }
}

bool menu_active(void) {
    return false;
}

static void draw_label(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float scale) {
    float width = (float)strlen(label) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale, height = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_RenderDebugText(renderer, floorf((rect->x + (rect->w - width) / 2) / scale), floorf((rect->y + (rect->h - height) / 2) / scale), label);
    SDL_SetRenderScale(renderer, 1, 1);
}

void menu_draw(SDL_Renderer *renderer) {
    layout_t current = layout();
    bool keyboard = SDL_ScreenKeyboardShown(main_window);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &current.buttons[i];
        bool lit = i == pressed || latched[i] || (BUTTONS[i].kind == BUTTON_KEYBOARD && keyboard) || (BUTTONS[i].kind == BUTTON_ITEM && checked[BUTTONS[i].item]);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, rect, BUTTONS[i].label, current.label_scale);
    }
}

void menu_ensure(void) {}

int menu_poll(void) {
    if (!queued) return -1;
    int item = queue[0];
    memmove(queue, queue + 1, (size_t)--queued * sizeof queue[0]);
    return item;
}

void menu_set_checked(int item, bool value) {
    if (item >= 0 && item < MENU_COUNT) checked[item] = value;
}

void menu_set_enabled(int item, bool value) {
    (void)item;
    (void)value;
}

void menu_set_title(int item, const char *title) {
    (void)item;
    (void)title;
}

void menu_set_hidden(int item, bool value) {
    if (item >= 0 && item < MENU_COUNT) hidden[item] = value;
}

int menu_modifiers(void) {
    return 0;
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    (void)window;
    (void)roms;
    (void)rom_count;
    (void)probe;
    (void)result;
    return false;
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    (void)window;
    (void)names;
    (void)count;
    (void)current;
    (void)chosen;
    return DIALOG_MANAGE_CLOSE;
}

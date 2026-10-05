#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "app/dialog.h"
#include "app/menu.h"

#define ROW_POINTS        44.0f
#define MIN_BUTTON_POINTS 52.0f
#define GAP_POINTS        3.0f
#define LABEL_HEIGHT      0.4f
#define MENU_QUEUE        16

typedef enum { BUTTON_KEYBOARD, BUTTON_KEY, BUTTON_MODIFIER, BUTTON_ITEM, BUTTON_NEXT_MACHINE } button_kind_t;

typedef struct {
    const char   *label;
    button_kind_t kind;
    SDL_Keycode   key;
    int           item;
} button_t;

static const button_t BUTTONS[] = {
    { "Kbd", BUTTON_KEYBOARD, 0, 0 },
    { "Esc", BUTTON_KEY, SDLK_ESCAPE, 0 },
    { "Tab", BUTTON_KEY, SDLK_TAB, 0 },
    { "Ctrl", BUTTON_MODIFIER, SDLK_LCTRL, 0 },
    { "Alt", BUTTON_MODIFIER, SDLK_LALT, 0 },
    { "Shift", BUTTON_MODIFIER, SDLK_LSHIFT, 0 },
    { "<", BUTTON_KEY, SDLK_LEFT, 0 },
    { "^", BUTTON_KEY, SDLK_UP, 0 },
    { "v", BUTTON_KEY, SDLK_DOWN, 0 },
    { ">", BUTTON_KEY, SDLK_RIGHT, 0 },
    { "Light", BUTTON_ITEM, 0, MENU_BACKLIGHT },
    { "Power", BUTTON_ITEM, 0, MENU_POWER },
    { "Switch", BUTTON_NEXT_MACHINE, 0, 0 },
};

#define BUTTON_COUNT (int)(sizeof BUTTONS / sizeof BUTTONS[0])

static SDL_Window *main_window;
static int         pressed = -1;
static bool        latched[BUTTON_COUNT];
static bool        checked[MENU_COUNT];
static bool        hidden[MENU_COUNT];
static int         queue[MENU_QUEUE];
static int         queued;

static float display_scale(void) {
    float scale = main_window ? SDL_GetWindowDisplayScale(main_window) : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return scale > 0 ? scale : 1.0f;
}

static void window_size(int *width, int *height) {
    *width = *height = 0;
    if (main_window) {
        SDL_GetWindowSize(main_window, width, height);
        return;
    }
    const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (mode) {
        *width = mode->w;
        *height = mode->h;
    }
}

static int row_count(void) {
    int width, height;
    window_size(&width, &height);
    float needed = BUTTON_COUNT * MIN_BUTTON_POINTS * display_scale();
    return width > 0 && (float)width < needed ? 2 : 1;
}

static int per_row(void) {
    int rows = row_count();
    return (BUTTON_COUNT + rows - 1) / rows;
}

static SDL_FRect button_rect(int index) {
    int width, height;
    window_size(&width, &height);
    float scale = display_scale();
    float row_height = ROW_POINTS * scale, gap = GAP_POINTS * scale;
    int columns = per_row();
    int row = index / columns, column = index % columns;
    float cell = (float)width / columns;
    return (SDL_FRect){ floorf(column * cell + gap / 2), floorf(row * row_height + gap / 2), floorf(cell - gap), floorf(row_height - gap) };
}

static int button_at(float x, float y) {
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect rect = button_rect(i);
        if (x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h) return i;
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
    return (int)(row_count() * ROW_POINTS * display_scale());
}

bool menu_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (event->button.y >= menu_bar_height()) return false;
        int index = button_at(event->button.x, event->button.y);
        if (index >= 0) press(index);
        return true;
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

static void draw_label(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label) {
    float length = (float)strlen(label);
    float scale = floorf(fminf(rect->h * LABEL_HEIGHT, rect->w * 0.8f / length) / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE);
    if (scale < 1) scale = 1;
    float width = length * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale, height = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_RenderDebugText(renderer, floorf((rect->x + (rect->w - width) / 2) / scale), floorf((rect->y + (rect->h - height) / 2) / scale), label);
    SDL_SetRenderScale(renderer, 1, 1);
}

void menu_draw(SDL_Renderer *renderer) {
    int width, height;
    window_size(&width, &height);
    SDL_SetRenderDrawColor(renderer, 0x22, 0x22, 0x22, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, (float)menu_bar_height() });
    bool keyboard = SDL_ScreenKeyboardShown(main_window);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect rect = button_rect(i);
        bool lit = i == pressed || latched[i] || (BUTTONS[i].kind == BUTTON_KEYBOARD && keyboard) || (BUTTONS[i].kind == BUTTON_ITEM && checked[BUTTONS[i].item]);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, &rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, &rect, BUTTONS[i].label);
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

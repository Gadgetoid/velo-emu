#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

#include "app/dialog.h"
#include "app/menu.h"
#include "app/menu_layout.h"

#define ROW_POINTS        44.0f
#define COLUMN_POINTS     64.0f
#define TAB_POINTS        28.0f
#define GAP_POINTS        3.0f
#define LIST_ROW_POINTS   48.0f
#define LIST_HEAD_POINTS  32.0f
#define LIST_SEP_POINTS   12.0f
#define LIST_PAD_POINTS   16.0f
#define DRAG_POINTS       8.0f
#define FLING_DECAY       4.0f
#define FLING_STOP_POINTS 20.0f
#define LABEL_WIDTH       0.85f
#define LABEL_HEIGHT      0.4f
#define LIST_LABEL_HEIGHT 0.3f
#define MENU_QUEUE        16
#define PAGE_MAX          8
#define ROW_MAX           64
#define TITLE_MAX         96
#define TAB_MAX           (PAGE_MAX + 1)
#define MODAL_WAIT_MS     16

typedef enum { BUTTON_TOGGLE, BUTTON_MENU, BUTTON_KEYBOARD, BUTTON_KEY, BUTTON_MODIFIER, BUTTON_ITEM } button_kind_t;

typedef struct {
    const char   *label;
    button_kind_t kind;
    SDL_Keycode   key;
    int           item;
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

static const int UNSUPPORTED[] = {
    MENU_SHOW_DEBUG_OUTPUT, MENU_SHOW_STATE, MENU_COPY_SCREEN,
    MENU_SAVE_SCREENSHOT, MENU_SCALE_50, MENU_SCALE_75, MENU_SCALE_100, MENU_SCALE_150, MENU_SCALE_200,
    MENU_ZOOM_IN, MENU_ZOOM_OUT, MENU_FULL_SCREEN, MENU_SERIAL_PTY, MENU_SHARED_FOLDER, MENU_SYNC_NOW,
    MENU_STOP_SHARING, MENU_FETCH_DOCUMENTS,
};

typedef struct {
    SDL_FRect buttons[BUTTON_COUNT];
    int       left, top, right, bottom;
    float     label_scale;
} layout_t;

typedef enum { ROW_ITEM, ROW_HEADING, ROW_SEPARATOR } row_kind_t;

typedef struct {
    row_kind_t  kind;
    int         tag;
    const char *title;
    bool        checked, disabled;
} row_t;

typedef struct {
    const char *tabs[TAB_MAX];
    int         tab_count, tab_selected;
    row_t       rows[ROW_MAX];
    int         row_count;
} list_t;

typedef struct {
    bool tapped;
    int  tab, tag;
} tap_t;

static SDL_Window *main_window;
static bool        collapsed;
static int         pressed = -1;
static bool        latched[BUTTON_COUNT];
static bool        checked[MENU_COUNT];
static bool        hidden[MENU_COUNT];
static bool        disabled[MENU_COUNT];
static char        titles[MENU_COUNT][TITLE_MAX];
static int         queue[MENU_QUEUE];
static int         queued;

static bool  panel_open;
static int   page;
static float scroll;
static bool  touching, dragging;
static float touch_x, touch_y, touch_scroll;
static float velocity, last_motion_y;
static uint64_t last_motion_ns, last_frame_ns;

static float display_scale(void) {
    float scale = main_window ? SDL_GetWindowDisplayScale(main_window) : 1.0f;
    return scale > 0 ? scale : 1.0f;
}

static SDL_FRect inset(SDL_FRect rect, float gap) {
    return (SDL_FRect){ floorf(rect.x + gap / 2), floorf(rect.y + gap / 2), floorf(rect.w - gap), floorf(rect.h - gap) };
}

static bool contains(const SDL_FRect *rect, float x, float y) {
    return rect->w > 0 && x >= rect->x && x < rect->x + rect->w && y >= rect->y && y < rect->y + rect->h;
}

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
    float scale = display_scale();
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
        result.buttons[i] = inset(cell, gap);
    }
    if (landscape) {
        result.left = leading;
        result.right = trailing;
    } else {
        result.top = leading;
        result.bottom = trailing;
    }
    result.label_scale = INFINITY;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &result.buttons[i];
        if (rect->w <= 0) continue;
        float fit = fminf(rect->h * LABEL_HEIGHT, rect->w * LABEL_WIDTH / (float)strlen(label_of(i))) / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        result.label_scale = fminf(result.label_scale, fit);
    }
    result.label_scale = fmaxf(1, floorf(result.label_scale));
    return result;
}

static int button_at(float x, float y) {
    layout_t current = layout();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (contains(&current.buttons[i], x, y)) return i;
    }
    return -1;
}

static bool supported(int tag) {
    if (tag >= MENU_SERIAL_PORT_FIRST && tag <= MENU_SERIAL_PORT_LAST) return false;
    for (size_t i = 0; i < sizeof UNSUPPORTED / sizeof UNSUPPORTED[0]; i++) {
        if (UNSUPPORTED[i] == tag) return false;
    }
    return true;
}

static int page_entries(int *starts) {
    int count = 0;
    for (int i = 0; i < MENU_ENTRY_COUNT && count < PAGE_MAX; i++) {
        if (MENU_ENTRIES[i].kind == MENU_ENTRY_MENU) starts[count++] = i;
    }
    return count;
}

static bool visible_item(const menu_entry_t *entry) {
    return entry->kind == MENU_ENTRY_ITEM && supported(entry->tag) && !hidden[entry->tag];
}

static bool section_has_items(int from) {
    for (int i = from; i < MENU_ENTRY_COUNT; i++) {
        menu_entry_kind_t kind = MENU_ENTRIES[i].kind;
        if (kind == MENU_ENTRY_END || kind == MENU_ENTRY_SEPARATOR || kind == MENU_ENTRY_HEADING || kind == MENU_ENTRY_SUBMENU) return false;
        if (visible_item(&MENU_ENTRIES[i])) return true;
    }
    return false;
}

static void add_row(list_t *list, row_kind_t kind, int tag, const char *title, bool checked, bool disabled) {
    if (list->row_count < ROW_MAX) list->rows[list->row_count++] = (row_t){ kind, tag, title, checked, disabled };
}

static void panel_list(list_t *list) {
    int starts[PAGE_MAX];
    int pages = page_entries(starts);
    list->tab_count = pages + 1;
    list->tab_selected = page;
    list->row_count = 0;
    for (int i = 0; i < pages; i++) list->tabs[i] = MENU_ENTRIES[starts[i]].title;
    list->tabs[pages] = "Close";
    if (page < 0 || page >= pages) return;
    int depth = 0;
    for (int i = starts[page] + 1; i < MENU_ENTRY_COUNT; i++) {
        const menu_entry_t *entry = &MENU_ENTRIES[i];
        if (entry->kind == MENU_ENTRY_END) {
            if (depth-- == 0) break;
            continue;
        }
        if (entry->kind == MENU_ENTRY_SUBMENU || entry->kind == MENU_ENTRY_HEADING) {
            if (entry->kind == MENU_ENTRY_SUBMENU) depth++;
            if (section_has_items(i + 1)) add_row(list, ROW_HEADING, 0, entry->title, false, false);
        } else if (entry->kind == MENU_ENTRY_SEPARATOR) {
            if (list->row_count && list->rows[list->row_count - 1].kind != ROW_SEPARATOR) add_row(list, ROW_SEPARATOR, 0, NULL, false, false);
        } else if (visible_item(entry)) {
            int tag = entry->tag;
            add_row(list, ROW_ITEM, tag, titles[tag][0] ? titles[tag] : entry->title, checked[tag], disabled[tag]);
        }
    }
    while (list->row_count && list->rows[list->row_count - 1].kind == ROW_SEPARATOR) list->row_count--;
}

static float row_height(row_kind_t kind) {
    float points = kind == ROW_ITEM ? LIST_ROW_POINTS : kind == ROW_HEADING ? LIST_HEAD_POINTS : LIST_SEP_POINTS;
    return floorf(points * display_scale());
}

static SDL_Rect list_area(void) {
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(main_window, &safe);
    if (width > height) return (SDL_Rect){ safe.x, 0, safe.w, height };
    return (SDL_Rect){ 0, safe.y, width, safe.h };
}

static SDL_FRect tab_rect(int index, int count) {
    SDL_Rect area = list_area();
    float cell = (float)area.w / count;
    return inset((SDL_FRect){ area.x + index * cell, (float)area.y, cell, floorf(ROW_POINTS * display_scale()) }, GAP_POINTS * display_scale());
}

static float list_top(void) {
    return (float)list_area().y + floorf(ROW_POINTS * display_scale()) + floorf(GAP_POINTS * display_scale());
}

static void list_reset(void) {
    scroll = velocity = 0;
    touching = dragging = false;
    last_frame_ns = 0;
}

static void clamp_scroll(const list_t *list) {
    float height = 0;
    for (int i = 0; i < list->row_count; i++) height += row_height(list->rows[i].kind);
    SDL_Rect area = list_area();
    float visible = (float)(area.y + area.h) - list_top();
    scroll = fminf(fmaxf(scroll, 0), fmaxf(0, height - visible));
}

static int row_at(const list_t *list, float y) {
    float top = list_top() - scroll;
    for (int i = 0; i < list->row_count; i++) {
        const row_t *row = &list->rows[i];
        float height = row_height(row->kind);
        if (y >= top && y < top + height) return row->kind == ROW_ITEM && !row->disabled ? row->tag : -1;
        top += height;
    }
    return -1;
}

static tap_t list_tap(const list_t *list, float x, float y) {
    tap_t tap = { true, -1, -1 };
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(i, list->tab_count);
        if (contains(&rect, x, y)) {
            tap.tab = i;
            return tap;
        }
    }
    if (y >= list_top()) tap.tag = row_at(list, y);
    return tap;
}

static tap_t list_event(const list_t *list, const SDL_Event *event) {
    tap_t tap = { false, -1, -1 };
    float threshold = DRAG_POINTS * display_scale();
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        touching = true;
        dragging = false;
        velocity = 0;
        touch_x = event->button.x;
        touch_y = event->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        if (!touching) break;
        if (!dragging && fabsf(event->motion.y - touch_y) > threshold && touch_y >= list_top()) {
            dragging = true;
            touch_y = last_motion_y = event->motion.y;
            touch_scroll = scroll;
            last_motion_ns = event->motion.timestamp;
        }
        if (!dragging) break;
        scroll = touch_scroll - (event->motion.y - touch_y);
        clamp_scroll(list);
        float seconds = (float)(event->motion.timestamp - last_motion_ns) / SDL_NS_PER_SECOND;
        if (seconds > 0) velocity = velocity * 0.5f + (last_motion_y - event->motion.y) / seconds * 0.5f;
        last_motion_y = event->motion.y;
        last_motion_ns = event->motion.timestamp;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (touching && !dragging) tap = list_tap(list, touch_x, touch_y);
        if (!dragging || (float)(event->button.timestamp - last_motion_ns) / SDL_NS_PER_SECOND > 0.1f) velocity = 0;
        touching = dragging = false;
        last_frame_ns = SDL_GetTicksNS();
        break;
    default:
        break;
    }
    return tap;
}

static void fling(const list_t *list) {
    uint64_t now = SDL_GetTicksNS();
    float seconds = last_frame_ns ? (float)(now - last_frame_ns) / SDL_NS_PER_SECOND : 0;
    last_frame_ns = now;
    if (touching || velocity == 0 || seconds <= 0) return;
    float before = scroll;
    scroll += velocity * seconds;
    clamp_scroll(list);
    velocity *= expf(-FLING_DECAY * seconds);
    if (scroll == before || fabsf(velocity) < FLING_STOP_POINTS * display_scale()) velocity = 0;
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

static void release(void) {
    if (pressed < 0) return;
    push_key(BUTTONS[pressed].key, false);
    pressed = -1;
}

static void open_panel(bool open) {
    panel_open = open;
    list_reset();
    if (open) {
        release();
        if (SDL_ScreenKeyboardShown(main_window)) SDL_StopTextInput(main_window);
    }
}

static void press(int index) {
    const button_t *button = &BUTTONS[index];
    switch (button->kind) {
    case BUTTON_TOGGLE:
        collapsed = !collapsed;
        break;
    case BUTTON_MENU:
        open_panel(true);
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
        enqueue(button->item);
        break;
    }
}

static bool panel_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key == SDLK_AC_BACK || event->key.key == SDLK_ESCAPE) open_panel(false);
        return true;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        return false;
    default:
        if (event->type >= SDL_EVENT_USER) return false;
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) return false;
        break;
    }
    list_t list;
    panel_list(&list);
    tap_t tap = list_event(&list, event);
    if (!tap.tapped) return true;
    if (tap.tab == list.tab_count - 1) {
        open_panel(false);
    } else if (tap.tab >= 0 && tap.tab != page) {
        page = tap.tab;
        list_reset();
    } else if (tap.tag >= 0) {
        enqueue(tap.tag);
        open_panel(false);
    }
    return true;
}

void menu_install(SDL_Window *window) {
    main_window = window;
    for (int i = 0; i < MENU_COUNT; i++) titles[i][0] = 0;
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
    if (panel_open) return panel_event(event);
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
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key != SDLK_AC_BACK) return false;
        open_panel(true);
        return true;
    case SDL_EVENT_KEY_UP:
        return event->key.key == SDLK_AC_BACK;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release();
        memset(latched, 0, sizeof latched);
        return false;
    default:
        return false;
    }
}

bool menu_active(void) {
    return panel_open;
}

static void copy_ascii(char *out, size_t size, const char *text) {
    size_t length = 0;
    for (const unsigned char *at = (const unsigned char *)text; *at && length + 4 < size; at++) {
        if (at[0] == 0xE2 && at[1] == 0x80 && at[2] == 0xA6) {
            memcpy(out + length, "...", 3);
            length += 3;
            at += 2;
        } else if (*at >= 0x20 && *at < 0x7F) {
            out[length++] = (char)*at;
        }
    }
    out[length] = 0;
}

static void draw_text(SDL_Renderer *renderer, float x, float y, const char *text, float scale) {
    char ascii[TITLE_MAX * 2];
    copy_ascii(ascii, sizeof ascii, text);
    SDL_SetRenderScale(renderer, scale, scale);
    SDL_RenderDebugText(renderer, floorf(x / scale), floorf(y / scale), ascii);
    SDL_SetRenderScale(renderer, 1, 1);
}

static void draw_label(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float scale) {
    float width = (float)strlen(label) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale, height = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale;
    draw_text(renderer, rect->x + (rect->w - width) / 2, rect->y + (rect->h - height) / 2, label, scale);
}

static void draw_list(SDL_Renderer *renderer, const list_t *list) {
    fling(list);
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, (float)height });

    float scale = display_scale();
    float character = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    float text_scale = fmaxf(1, floorf(LIST_ROW_POINTS * scale * LIST_LABEL_HEIGHT / character));
    float heading_scale = fmaxf(1, text_scale - 1);
    SDL_Rect area = list_area();
    float top = list_top();
    float pad = floorf(LIST_PAD_POINTS * scale);
    float mark = floorf(character * text_scale);
    float y = top - scroll;
    for (int i = 0; i < list->row_count; i++) {
        const row_t *row = &list->rows[i];
        float row_h = row_height(row->kind);
        if (row->kind == ROW_SEPARATOR) {
            SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
            SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + row_h / 2), area.w - 2 * pad, fmaxf(1, floorf(scale)) });
        } else if (row->kind == ROW_HEADING) {
            SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
            draw_text(renderer, area.x + pad, y + row_h - character * heading_scale - floorf(4 * scale), row->title, heading_scale);
        } else {
            if (row->checked) {
                SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
                SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + (row_h - mark) / 2), mark, mark });
            }
            if (row->disabled) SDL_SetRenderDrawColor(renderer, 0x66, 0x66, 0x66, 0xFF);
            else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
            draw_text(renderer, area.x + 2 * pad + mark, floorf(y + (row_h - character * text_scale) / 2), row->title, text_scale);
        }
        y += row_h;
    }

    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, top });
    float tab_scale = text_scale;
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(i, list->tab_count);
        tab_scale = fminf(tab_scale, fmaxf(1, floorf(rect.w * LABEL_WIDTH / (strlen(list->tabs[i]) * character))));
    }
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(i, list->tab_count);
        bool lit = i == list->tab_selected;
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, &rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, &rect, list->tabs[i], tab_scale);
    }
}

void menu_draw(SDL_Renderer *renderer) {
    layout_t current = layout();
    bool keyboard = SDL_ScreenKeyboardShown(main_window);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &current.buttons[i];
        if (rect->w <= 0) continue;
        bool lit = i == pressed || latched[i] || (BUTTONS[i].kind == BUTTON_KEYBOARD && keyboard) || (BUTTONS[i].kind == BUTTON_ITEM && checked[BUTTONS[i].item]);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, rect, label_of(i), current.label_scale);
    }
    if (panel_open) {
        list_t list;
        panel_list(&list);
        draw_list(renderer, &list);
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
    if (item >= 0 && item < MENU_COUNT) disabled[item] = !value;
}

void menu_set_title(int item, const char *title) {
    if (item >= 0 && item < MENU_COUNT) SDL_strlcpy(titles[item], title, sizeof titles[item]);
}

void menu_set_hidden(int item, bool value) {
    if (item >= 0 && item < MENU_COUNT) hidden[item] = value;
}

int menu_modifiers(void) {
    return 0;
}

static tap_t modal_step(const list_t *list, int cancel_tab) {
    SDL_Renderer *renderer = SDL_GetRenderer(main_window);
    draw_list(renderer, list);
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
        return list_event(list, &event);
    }
}

enum { CHOICE_ROM = 0x1000, CHOICE_MEMORY = 0x2000, CHOICE_SCREEN = 0x3000, CHOICE_CLOCK = 0x4000, CHOICE_MACHINE = 0x5000 };

static bool rom_screen(const dialog_rom_t *rom, int preset) {
    return rom->screens & (1u << preset);
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    (void)window;
    (void)probe;
    if (rom_count <= 0) return false;
    int rom = 0;
    for (int i = 0; i < rom_count; i++) {
        if (!strcmp(roms[i].path, result->rom)) rom = i;
    }
    int screen = screen_preset_index(result->screen);
    if (screen < 0 || !rom_screen(&roms[rom], screen)) screen = 0;
    uint32_t memory = result->memory;
    bool host_time = result->host_time;
    static char labels[ROW_MAX][TITLE_MAX];
    list_reset();
    for (;;) {
        list_t list = { { "Cancel", "Create" }, 2, -1, { { 0 } }, 0 };
        int label = 0;
        add_row(&list, ROW_HEADING, 0, "ROM", false, false);
        for (int i = 0; i < rom_count; i++) add_row(&list, ROW_ITEM, CHOICE_ROM + i, roms[i].label, i == rom, false);
        add_row(&list, ROW_HEADING, 0, "Memory", false, false);
        for (int i = 0; i < DIALOG_MEMORY_COUNT && label < ROW_MAX; i++) {
            snprintf(labels[label], TITLE_MAX, "%u MB", (unsigned)DIALOG_MEMORY_SIZES[i]);
            add_row(&list, ROW_ITEM, CHOICE_MEMORY + i, labels[label++], DIALOG_MEMORY_SIZES[i] == memory, false);
        }
        add_row(&list, ROW_HEADING, 0, "Screen", false, false);
        for (int i = 0; i < SCREEN_PRESET_COUNT && label < ROW_MAX; i++) {
            if (!rom_screen(&roms[rom], i)) continue;
            snprintf(labels[label], TITLE_MAX, "%dx%d", SCREEN_PRESETS[i].width, SCREEN_PRESETS[i].height);
            add_row(&list, ROW_ITEM, CHOICE_SCREEN + i, labels[label++], i == screen, false);
        }
        add_row(&list, ROW_HEADING, 0, "Clock", false, false);
        add_row(&list, ROW_ITEM, CHOICE_CLOCK, "Set the clock from this phone", host_time, false);
        tap_t tap = modal_step(&list, 0);
        if (!tap.tapped) continue;
        if (tap.tab == 0) return false;
        if (tap.tab == 1) {
            result->name[0] = 0;
            SDL_strlcpy(result->rom, roms[rom].path, sizeof result->rom);
            result->screen = SCREEN_PRESETS[screen];
            result->memory = memory;
            result->host_time = host_time;
            return true;
        }
        if (tap.tag >= CHOICE_ROM && tap.tag < CHOICE_ROM + rom_count) {
            rom = tap.tag - CHOICE_ROM;
            if (!rom_screen(&roms[rom], screen)) screen = 0;
        } else if (tap.tag >= CHOICE_MEMORY && tap.tag < CHOICE_MEMORY + DIALOG_MEMORY_COUNT) {
            memory = DIALOG_MEMORY_SIZES[tap.tag - CHOICE_MEMORY];
        } else if (tap.tag >= CHOICE_SCREEN && tap.tag < CHOICE_SCREEN + SCREEN_PRESET_COUNT) {
            screen = tap.tag - CHOICE_SCREEN;
        } else if (tap.tag == CHOICE_CLOCK) {
            host_time = !host_time;
        }
    }
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    (void)window;
    (void)current;
    int selected = *chosen >= 0 && *chosen < count ? *chosen : 0;
    list_reset();
    for (;;) {
        list_t list = { { "Close", "Reset", "Delete" }, 3, -1, { { 0 } }, 0 };
        add_row(&list, ROW_HEADING, 0, "Machines", false, false);
        for (int i = 0; i < count; i++) add_row(&list, ROW_ITEM, CHOICE_MACHINE + i, names[i], i == selected, false);
        tap_t tap = modal_step(&list, 0);
        if (!tap.tapped) continue;
        if (tap.tab == 0) return DIALOG_MANAGE_CLOSE;
        if (tap.tab == 1 || tap.tab == 2) {
            *chosen = selected;
            return tap.tab == 1 ? DIALOG_MANAGE_RESET : DIALOG_MANAGE_DELETE;
        }
        if (tap.tag >= CHOICE_MACHINE && tap.tag < CHOICE_MACHINE + count) selected = tap.tag - CHOICE_MACHINE;
    }
}

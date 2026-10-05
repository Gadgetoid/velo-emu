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
#define LIST_LABEL_HEIGHT 0.33f
#define MENU_QUEUE        16
#define PAGE_MAX          8
#define ROW_MAX           64
#define TITLE_MAX         96

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
    MENU_NEW_MACHINE, MENU_MANAGE_MACHINES, MENU_SHOW_DEBUG_OUTPUT, MENU_SHOW_STATE, MENU_COPY_SCREEN,
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
} row_t;

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

static int page_rows(int index, row_t *rows) {
    int starts[PAGE_MAX];
    int pages = page_entries(starts);
    if (index < 0 || index >= pages) return 0;
    int count = 0, depth = 0;
    for (int i = starts[index] + 1; i < MENU_ENTRY_COUNT && count < ROW_MAX; i++) {
        const menu_entry_t *entry = &MENU_ENTRIES[i];
        if (entry->kind == MENU_ENTRY_END) {
            if (depth-- == 0) break;
            continue;
        }
        if (entry->kind == MENU_ENTRY_SUBMENU) {
            depth++;
            if (section_has_items(i + 1)) rows[count++] = (row_t){ ROW_HEADING, 0, entry->title };
        } else if (entry->kind == MENU_ENTRY_HEADING) {
            if (section_has_items(i + 1)) rows[count++] = (row_t){ ROW_HEADING, 0, entry->title };
        } else if (entry->kind == MENU_ENTRY_SEPARATOR) {
            if (count && rows[count - 1].kind != ROW_SEPARATOR) rows[count++] = (row_t){ ROW_SEPARATOR, 0, NULL };
        } else if (visible_item(entry)) {
            rows[count++] = (row_t){ ROW_ITEM, entry->tag, titles[entry->tag][0] ? titles[entry->tag] : entry->title };
        }
    }
    while (count && rows[count - 1].kind == ROW_SEPARATOR) count--;
    return count;
}

static float row_height(row_kind_t kind) {
    float points = kind == ROW_ITEM ? LIST_ROW_POINTS : kind == ROW_HEADING ? LIST_HEAD_POINTS : LIST_SEP_POINTS;
    return floorf(points * display_scale());
}

static SDL_Rect panel_area(void) {
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(main_window, &safe);
    if (width > height) return (SDL_Rect){ safe.x, 0, safe.w, height };
    return (SDL_Rect){ 0, safe.y, width, safe.h };
}

static SDL_FRect tab_rect(int index, int count) {
    SDL_Rect area = panel_area();
    float cell = (float)area.w / count;
    return inset((SDL_FRect){ area.x + index * cell, (float)area.y, cell, floorf(ROW_POINTS * display_scale()) }, GAP_POINTS * display_scale());
}

static float list_top(void) {
    return (float)panel_area().y + floorf(ROW_POINTS * display_scale()) + floorf(GAP_POINTS * display_scale());
}

static float content_height(const row_t *rows, int count) {
    float height = 0;
    for (int i = 0; i < count; i++) height += row_height(rows[i].kind);
    return height;
}

static void clamp_scroll(void) {
    row_t rows[ROW_MAX];
    int count = page_rows(page, rows);
    SDL_Rect area = panel_area();
    float visible = (float)(area.y + area.h) - list_top();
    float limit = fmaxf(0, content_height(rows, count) - visible);
    scroll = fminf(fmaxf(scroll, 0), limit);
}

static int row_at(float y) {
    row_t rows[ROW_MAX];
    int count = page_rows(page, rows);
    float top = list_top() - scroll;
    for (int i = 0; i < count; i++) {
        float height = row_height(rows[i].kind);
        if (y >= top && y < top + height) return rows[i].kind == ROW_ITEM ? rows[i].tag : -1;
        top += height;
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

static void release(void) {
    if (pressed < 0) return;
    push_key(BUTTONS[pressed].key, false);
    pressed = -1;
}

static void open_panel(bool open) {
    panel_open = open;
    touching = dragging = false;
    velocity = 0;
    if (open) {
        release();
        if (SDL_ScreenKeyboardShown(main_window)) SDL_StopTextInput(main_window);
        clamp_scroll();
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

static void panel_tap(float x, float y) {
    int starts[PAGE_MAX];
    int pages = page_entries(starts);
    for (int i = 0; i <= pages; i++) {
        SDL_FRect rect = tab_rect(i, pages + 1);
        if (!contains(&rect, x, y)) continue;
        if (i == pages) open_panel(false);
        else if (i != page) {
            page = i;
            scroll = 0;
        }
        return;
    }
    if (y < list_top()) return;
    int tag = row_at(y);
    if (tag < 0 || disabled[tag]) return;
    enqueue(tag);
    open_panel(false);
}

static bool panel_event(const SDL_Event *event) {
    float threshold = DRAG_POINTS * display_scale();
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        touching = true;
        dragging = false;
        velocity = 0;
        touch_x = event->button.x;
        touch_y = event->button.y;
        touch_scroll = scroll;
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
        clamp_scroll();
        float seconds = (float)(event->motion.timestamp - last_motion_ns) / SDL_NS_PER_SECOND;
        if (seconds > 0) velocity = velocity * 0.5f + (last_motion_y - event->motion.y) / seconds * 0.5f;
        last_motion_y = event->motion.y;
        last_motion_ns = event->motion.timestamp;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (touching && !dragging) panel_tap(touch_x, touch_y);
        if (!dragging || (float)(event->button.timestamp - last_motion_ns) / SDL_NS_PER_SECOND > 0.1f) velocity = 0;
        touching = dragging = false;
        last_frame_ns = SDL_GetTicksNS();
        break;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key == SDLK_AC_BACK || event->key.key == SDLK_ESCAPE) open_panel(false);
        break;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        return false;
    default:
        if (event->type >= SDL_EVENT_USER) return false;
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) return false;
        break;
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

static void fling(void) {
    uint64_t now = SDL_GetTicksNS();
    float seconds = last_frame_ns ? (float)(now - last_frame_ns) / SDL_NS_PER_SECOND : 0;
    last_frame_ns = now;
    if (touching || velocity == 0 || seconds <= 0) return;
    float before = scroll;
    scroll += velocity * seconds;
    clamp_scroll();
    velocity *= expf(-FLING_DECAY * seconds);
    if (scroll == before || fabsf(velocity) < FLING_STOP_POINTS * display_scale()) velocity = 0;
}

static void draw_panel(SDL_Renderer *renderer) {
    fling();
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xF0);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, (float)height });

    float scale = display_scale();
    float text_scale = fmaxf(1, floorf(LIST_ROW_POINTS * scale * LIST_LABEL_HEIGHT / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE));
    float heading_scale = fmaxf(1, text_scale - 1);
    float character = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    SDL_Rect area = panel_area();
    float top = list_top();
    float pad = floorf(LIST_PAD_POINTS * scale);
    float mark = floorf(character * text_scale);
    float y = top - scroll;
    row_t rows[ROW_MAX];
    int count = page_rows(page, rows);
    for (int i = 0; i < count; i++) {
        float height = row_height(rows[i].kind);
        if (rows[i].kind == ROW_SEPARATOR) {
            SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
            SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + height / 2), area.w - 2 * pad, fmaxf(1, floorf(scale)) });
        } else if (rows[i].kind == ROW_HEADING) {
            SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
            draw_text(renderer, area.x + pad, y + height - character * heading_scale - floorf(4 * scale), rows[i].title, heading_scale);
        } else {
            int tag = rows[i].tag;
            if (checked[tag]) {
                SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
                SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + (height - mark) / 2), mark, mark });
            }
            if (disabled[tag]) SDL_SetRenderDrawColor(renderer, 0x66, 0x66, 0x66, 0xFF);
            else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
            draw_text(renderer, area.x + 2 * pad + mark, floorf(y + (height - character * text_scale) / 2), rows[i].title, text_scale);
        }
        y += height;
    }

    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, top });
    int starts[PAGE_MAX];
    int pages = page_entries(starts);
    float tab_scale = text_scale;
    for (int i = 0; i <= pages; i++) {
        SDL_FRect rect = tab_rect(i, pages + 1);
        const char *title = i == pages ? "Close" : MENU_ENTRIES[starts[i]].title;
        tab_scale = fminf(tab_scale, fmaxf(1, floorf(rect.w * LABEL_WIDTH / (strlen(title) * character))));
    }
    for (int i = 0; i <= pages; i++) {
        SDL_FRect rect = tab_rect(i, pages + 1);
        bool lit = i == page;
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, &rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, &rect, i == pages ? "Close" : MENU_ENTRIES[starts[i]].title, tab_scale);
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
    if (panel_open) draw_panel(renderer);
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

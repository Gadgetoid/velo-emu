#include "frontend/common/menu_layout.h"
#include "frontend/common/menu_queue.h"
#include "frontend/common/menu_state.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "frontend/linux/ui.h"

#define BAR_HEIGHT       24.0f
#define BAR_INSET        4.0f
#define BAR_PADDING      9.0f
#define ROW_HEIGHT       24.0f
#define SEPARATOR_HEIGHT 9.0f
#define HEADING_HEIGHT   20.0f
#define HEADING_INSET    12.0f
#define MENU_PADDING     4.0f
#define CHECK_COLUMN     26.0f
#define ARROW_COLUMN     22.0f
#define TEXT_RIGHT       14.0f
#define SHORTCUT_GAP     32.0f
#define MENU_MIN_WIDTH   160.0f
#define SUBMENU_OVERLAP  3.0f

#define MAX_MENUS   16
#define MAX_ITEMS   192
#define MENU_ITEMS  48
#define MAX_DEPTH   4

typedef enum { ITEM_ACTION, ITEM_SEPARATOR, ITEM_SUBMENU, ITEM_HEADING } item_kind_t;

typedef struct {
    item_kind_t kind;
    int tag;
    int submenu;
    char title[96];
    char shortcut[32];
    SDL_Keycode key;
    SDL_Keymod modifiers;
} item_t;

typedef struct {
    char title[32];
    int first, count;
    float bar_x, bar_width;
} menu_t;

typedef struct {
    int menu;
    int hover;
    float width, height;
    canvas_t canvas;
} level_t;

static SDL_Window *main_window;
static canvas_t bar_canvas;
static menu_t menus[MAX_MENUS];
static int menu_count;
static int top_menus[MAX_MENUS];
static int top_count;
static item_t items[MAX_ITEMS];
static int item_count;
static level_t levels[MAX_DEPTH];
static int depth;
static int open_top = -1;
static bool popup_failed;

static void set_shortcut(item_t *item, const menu_entry_t *entry) {
    if (entry->tag == MENU_FULL_SCREEN) {
        item->key = SDLK_F11;
        item->modifiers = SDL_KMOD_NONE;
        snprintf(item->shortcut, sizeof item->shortcut, "F11");
        return;
    }
    if (!entry->key || (entry->modifiers & MENU_KEY_CONTROL)) return;
    bool shift = (entry->modifiers & MENU_KEY_SHIFT) != 0;
    item->key = (SDL_Keycode)(unsigned char)entry->key;
    item->modifiers = SDL_KMOD_CTRL | SDL_KMOD_ALT | (shift ? SDL_KMOD_SHIFT : 0);
    snprintf(item->shortcut, sizeof item->shortcut, "%sCtrl+Alt+%c", shift ? "Shift+" : "", toupper((unsigned char)entry->key));
}

static void skip_menu(int *cursor) {
    for (int nesting = 1; *cursor < MENU_ENTRY_COUNT && nesting > 0; (*cursor)++) {
        menu_entry_kind_t kind = MENU_ENTRIES[*cursor].kind;
        if (kind == MENU_ENTRY_SUBMENU) nesting++;
        else if (kind == MENU_ENTRY_END) nesting--;
    }
}

static int parse_menu(int *cursor, const char *title) {
    if (menu_count >= MAX_MENUS) {
        skip_menu(cursor);
        return -1;
    }
    item_t local[MENU_ITEMS];
    int local_count = 0;
    int index = menu_count++;
    snprintf(menus[index].title, sizeof menus[index].title, "%s", title);
    while (*cursor < MENU_ENTRY_COUNT) {
        const menu_entry_t *entry = &MENU_ENTRIES[(*cursor)++];
        if (entry->kind == MENU_ENTRY_END) break;
        item_t item = { ITEM_ACTION, -1, -1, "", "", 0, SDL_KMOD_NONE };
        if (entry->kind == MENU_ENTRY_SEPARATOR) {
            item.kind = ITEM_SEPARATOR;
        } else if (entry->kind == MENU_ENTRY_HEADING) {
            item.kind = ITEM_HEADING;
            snprintf(item.title, sizeof item.title, "%s", entry->title);
        } else if (entry->kind == MENU_ENTRY_SUBMENU) {
            item.kind = ITEM_SUBMENU;
            snprintf(item.title, sizeof item.title, "%s", entry->title);
            item.submenu = parse_menu(cursor, entry->title);
            if (item.submenu < 0) continue;
        } else if (entry->kind == MENU_ENTRY_ITEM) {
            item.tag = entry->tag;
            snprintf(item.title, sizeof item.title, "%s", entry->title);
            set_shortcut(&item, entry);
        } else {
            continue;
        }
        if (local_count < MENU_ITEMS) local[local_count++] = item;
    }
    menus[index].first = item_count;
    menus[index].count = 0;
    for (int i = 0; i < local_count && item_count < MAX_ITEMS; i++) {
        items[item_count++] = local[i];
        menus[index].count++;
    }
    return index;
}

static const char *item_title(const item_t *item) {
    return item->kind == ITEM_ACTION ? menu_state_title(item->tag, item->title) : item->title;
}

static bool item_visible(const item_t *item) {
    return item->kind != ITEM_ACTION || !menu_state_hidden(item->tag);
}

static bool item_selectable(const item_t *item) {
    if (item->kind == ITEM_SEPARATOR || item->kind == ITEM_HEADING || !item_visible(item)) return false;
    return item->kind == ITEM_SUBMENU || menu_state_enabled(item->tag);
}

static float item_height(const item_t *item) {
    if (item->kind == ITEM_SEPARATOR) return SEPARATOR_HEIGHT;
    return item->kind == ITEM_HEADING ? HEADING_HEIGHT : ROW_HEIGHT;
}

static void measure_menu(int menu, float *width, float *height) {
    float title_width = 0, shortcut_width = 0, rows = 0;
    bool has_submenu = false;
    for (int i = menus[menu].first; i < menus[menu].first + menus[menu].count; i++) {
        const item_t *item = &items[i];
        if (!item_visible(item)) continue;
        rows += item_height(item);
        if (item->kind == ITEM_SEPARATOR) continue;
        title_width = fmaxf(title_width, ui_text_width(item_title(item)));
        if (item->shortcut[0]) shortcut_width = fmaxf(shortcut_width, ui_text_width(item->shortcut));
        if (item->kind == ITEM_SUBMENU) has_submenu = true;
    }
    float total = CHECK_COLUMN + title_width + (shortcut_width > 0 ? SHORTCUT_GAP + shortcut_width : 0) + (has_submenu ? ARROW_COLUMN : TEXT_RIGHT);
    *width = ceilf(fmaxf(total, MENU_MIN_WIDTH));
    *height = ceilf(rows + 2 * MENU_PADDING);
}

static void layout_bar(void) {
    float x = BAR_INSET;
    for (int i = 0; i < top_count; i++) {
        menu_t *menu = &menus[top_menus[i]];
        menu->bar_x = x;
        menu->bar_width = ceilf(ui_text_width(menu->title) + 2 * BAR_PADDING);
        x += menu->bar_width;
    }
}

static void level_origin(int level, float *x, float *y) {
    *x = 0;
    *y = 0;
    for (int i = 0; i <= level && i < depth; i++) {
        int offset_x = 0, offset_y = 0;
        SDL_GetWindowPosition(levels[i].canvas.window, &offset_x, &offset_y);
        *x += (float)offset_x;
        *y += (float)offset_y;
    }
}

static float item_top(const level_t *level, int index) {
    float y = MENU_PADDING;
    for (int i = menus[level->menu].first; i < index; i++) {
        if (item_visible(&items[i])) y += item_height(&items[i]);
    }
    return y;
}

static void close_levels(int from) {
    while (depth > from) ui_destroy_canvas(&levels[--depth].canvas);
    if (depth == 0) open_top = -1;
}

static bool open_level(int level, int menu, float x, float y) {
    close_levels(level);
    float width, height;
    measure_menu(menu, &width, &height);
    float parent_x = 0, parent_y = 0;
    if (level > 0) level_origin(level - 1, &parent_x, &parent_y);
    SDL_Window *parent = level > 0 ? levels[level - 1].canvas.window : main_window;
    SDL_Window *window = SDL_CreatePopupWindow(parent, (int)(x - parent_x), (int)(y - parent_y), (int)width, (int)height,
                                               SDL_WINDOW_POPUP_MENU | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER) : NULL;
    if (!renderer) {
        if (!popup_failed) fprintf(stderr, "menu: %s\n", SDL_GetError());
        popup_failed = true;
        if (window) SDL_DestroyWindow(window);
        return false;
    }
    levels[level] = (level_t){ menu, -1, width, height, { window, renderer, NULL, 0 } };
    depth = level + 1;
    return true;
}

static int first_selectable(int menu, int from, int step) {
    int count = menus[menu].count;
    for (int i = 0; i < count; i++) {
        int index = menus[menu].first + ((from - menus[menu].first + step * (i + 1)) % count + count) % count;
        if (item_selectable(&items[index])) return index;
    }
    return -1;
}

static void open_top_menu(int top, bool select_first) {
    close_levels(0);
    layout_bar();
    if (!open_level(0, top_menus[top], menus[top_menus[top]].bar_x, BAR_HEIGHT)) return;
    open_top = top;
    if (select_first) levels[0].hover = first_selectable(levels[0].menu, menus[levels[0].menu].first - 1, 1);
}

static void open_submenu(int level, int index) {
    if (items[index].kind != ITEM_SUBMENU) return;
    if (depth > level + 1 && levels[level + 1].menu == items[index].submenu) return;
    float origin_x, origin_y;
    level_origin(level, &origin_x, &origin_y);
    open_level(level + 1, items[index].submenu, origin_x + levels[level].width - SUBMENU_OVERLAP, origin_y + item_top(&levels[level], index) - MENU_PADDING);
}

static void activate(int index) {
    const item_t *item = &items[index];
    if (!item_selectable(item)) return;
    if (item->kind == ITEM_SUBMENU) {
        for (int level = 0; level < depth; level++) {
            if (levels[level].hover == index) {
                open_submenu(level, index);
                if (depth > level + 1) levels[level + 1].hover = first_selectable(levels[level + 1].menu, menus[levels[level + 1].menu].first - 1, 1);
            }
        }
        return;
    }
    close_levels(0);
    menu_queue_push(item->tag);
}

static int bar_hit(float x, float y) {
    if (y < 0 || y >= BAR_HEIGHT) return -1;
    layout_bar();
    for (int i = 0; i < top_count; i++) {
        const menu_t *menu = &menus[top_menus[i]];
        if (x >= menu->bar_x && x < menu->bar_x + menu->bar_width) return i;
    }
    return -1;
}

static int level_hit(float x, float y, int *index) {
    for (int level = depth - 1; level >= 0; level--) {
        float origin_x, origin_y;
        level_origin(level, &origin_x, &origin_y);
        float local_x = x - origin_x, local_y = y - origin_y;
        if (local_x < 0 || local_y < 0 || local_x >= levels[level].width || local_y >= levels[level].height) continue;
        *index = -1;
        float top = MENU_PADDING;
        const menu_t *menu = &menus[levels[level].menu];
        for (int i = menu->first; i < menu->first + menu->count; i++) {
            if (!item_visible(&items[i])) continue;
            float height = item_height(&items[i]);
            if (local_y >= top && local_y < top + height) *index = i;
            top += height;
        }
        return level;
    }
    return -1;
}

static int level_of_window(SDL_WindowID id) {
    for (int level = 0; level < depth; level++) {
        if (SDL_GetWindowID(levels[level].canvas.window) == id) return level;
    }
    return -1;
}

static bool to_main_point(SDL_WindowID id, float x, float y, float *main_x, float *main_y) {
    if (main_window && id == SDL_GetWindowID(main_window)) {
        *main_x = x;
        *main_y = y;
        return true;
    }
    int level = level_of_window(id);
    if (level < 0) return false;
    float origin_x, origin_y;
    level_origin(level, &origin_x, &origin_y);
    *main_x = x + origin_x;
    *main_y = y + origin_y;
    return true;
}

static void hover_at(float x, float y) {
    int top = bar_hit(x, y);
    if (top >= 0) {
        if (top != open_top) open_top_menu(top, false);
        return;
    }
    int index;
    int level = level_hit(x, y, &index);
    if (level < 0) return;
    levels[level].hover = index >= 0 && item_selectable(&items[index]) ? index : -1;
    if (levels[level].hover >= 0 && items[index].kind == ITEM_SUBMENU) open_submenu(level, index);
    else close_levels(level + 1);
}

static bool mouse_event(const SDL_Event *event) {
    float x, y;
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION;
    SDL_WindowID id = motion ? event->motion.windowID : event->button.windowID;
    float event_x = motion ? event->motion.x : event->button.x, event_y = motion ? event->motion.y : event->button.y;
    bool in_main = main_window && id == SDL_GetWindowID(main_window);
    if (!to_main_point(id, event_x, event_y, &x, &y)) return !in_main;
    if (!depth) {
        if (!in_main) return true;
        if (event->type != SDL_EVENT_MOUSE_BUTTON_DOWN || y >= BAR_HEIGHT) return false;
        int top = bar_hit(x, y);
        if (top >= 0) open_top_menu(top, false);
        return true;
    }
    if (motion) {
        hover_at(x, y);
        return true;
    }
    int index;
    int level = level_hit(x, y, &index);
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int top = bar_hit(x, y);
        if (top >= 0) {
            if (top == open_top) close_levels(0);
            else open_top_menu(top, false);
        } else if (level < 0) {
            close_levels(0);
        }
        return true;
    }
    if (level >= 0 && index >= 0 && items[index].kind == ITEM_ACTION) activate(index);
    return true;
}

static void move_top(int step) {
    if (top_count) open_top_menu(((open_top + step) % top_count + top_count) % top_count, true);
}

static bool key_event(const SDL_Event *event) {
    if (!depth) {
        if (event->type != SDL_EVENT_KEY_DOWN) return false;
        SDL_Keycode key = SDL_GetKeyFromScancode(event->key.scancode, SDL_KMOD_NONE, false);
        SDL_Keymod modifiers = SDL_KMOD_NONE;
        if (event->key.mod & SDL_KMOD_CTRL) modifiers |= SDL_KMOD_CTRL;
        if (event->key.mod & SDL_KMOD_ALT) modifiers |= SDL_KMOD_ALT;
        if (event->key.mod & SDL_KMOD_SHIFT) modifiers |= SDL_KMOD_SHIFT;
        if (key == SDLK_F10 && modifiers == SDL_KMOD_NONE) {
            if (!event->key.repeat) open_top_menu(0, true);
            return true;
        }
        for (int i = 0; i < item_count; i++) {
            if (!items[i].key || items[i].key != key || items[i].modifiers != modifiers) continue;
            if (!event->key.repeat && menu_state_enabled(items[i].tag)) menu_queue_push(items[i].tag);
            return true;
        }
        return false;
    }
    if (event->type != SDL_EVENT_KEY_DOWN) return true;
    level_t *level = &levels[depth - 1];
    switch (event->key.key) {
    case SDLK_ESCAPE:
    case SDLK_F10:
        close_levels(event->key.key == SDLK_ESCAPE && depth > 1 ? depth - 1 : 0);
        break;
    case SDLK_UP:
    case SDLK_DOWN: {
        bool down = event->key.key == SDLK_DOWN;
        int from = level->hover;
        if (from < 0) from = down ? menus[level->menu].first - 1 : menus[level->menu].first;
        int next = first_selectable(level->menu, from, down ? 1 : -1);
        if (next >= 0) level->hover = next;
        close_levels((int)(level - levels) + 1);
        break;
    }
    case SDLK_RIGHT:
        if (level->hover >= 0 && items[level->hover].kind == ITEM_SUBMENU) activate(level->hover);
        else move_top(1);
        break;
    case SDLK_LEFT:
        if (depth > 1) close_levels(depth - 1);
        else move_top(-1);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        if (level->hover >= 0) activate(level->hover);
        break;
    default:
        break;
    }
    return true;
}

void menu_install(SDL_Window *window) {
    main_window = window;
    for (int cursor = 0; cursor < MENU_ENTRY_COUNT;) {
        const menu_entry_t *entry = &MENU_ENTRIES[cursor++];
        if (entry->kind != MENU_ENTRY_MENU) continue;
        int menu = parse_menu(&cursor, entry->title);
        if (menu >= 0) top_menus[top_count++] = menu;
    }
    ui_load_font();
    ui_prepare(main_window);
}

void menu_ensure(void) {
}

int menu_bar_height(void) {
    return (int)BAR_HEIGHT;
}

void menu_insets(int *left, int *top, int *right, int *bottom) {
    *left = *right = *bottom = 0;
    *top = menu_bar_height();
}

bool menu_active(void) {
    return depth > 0;
}

bool menu_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return mouse_event(event);
    case SDL_EVENT_MOUSE_WHEEL:
        return level_of_window(event->wheel.windowID) >= 0;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return key_event(event);
    default:
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST && level_of_window(event->window.windowID) >= 0) {
            if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) close_levels(0);
            return true;
        }
        return false;
    }
}

static void draw_bar(SDL_Renderer *renderer) {
    const palette_t *colours = ui_palette();
    bar_canvas.renderer = renderer;
    int width = 0, height = 0;
    SDL_GetWindowSize(main_window, &width, &height);
    layout_bar();
    ui_fill(&bar_canvas, 0, 0, (float)width, BAR_HEIGHT, colours->bar);
    ui_fill(&bar_canvas, 0, BAR_HEIGHT - 1, (float)width, 1, colours->bar_border);
    for (int i = 0; i < top_count; i++) {
        const menu_t *menu = &menus[top_menus[i]];
        if (i == open_top) ui_fill(&bar_canvas, menu->bar_x, 0, menu->bar_width, BAR_HEIGHT - 1, colours->bar_open);
        ui_text(&bar_canvas, menu->bar_x + BAR_PADDING, 0, BAR_HEIGHT - 1, menu->title, colours->text);
    }
}

static void draw_level(level_t *level) {
    const palette_t *colours = ui_palette();
    canvas_t *canvas = &level->canvas;
    float width, height;
    measure_menu(level->menu, &width, &height);
    if (width != level->width || height != level->height) {
        level->width = width;
        level->height = height;
        SDL_SetWindowSize(canvas->window, (int)width, (int)height);
    }
    ui_fill(canvas, 0, 0, width, height, colours->menu_border);
    ui_fill(canvas, 1, 1, width - 2, height - 2, colours->menu);
    float top = MENU_PADDING;
    const menu_t *menu = &menus[level->menu];
    for (int i = menu->first; i < menu->first + menu->count; i++) {
        const item_t *item = &items[i];
        if (!item_visible(item)) continue;
        if (item->kind == ITEM_SEPARATOR) {
            ui_fill(canvas, 1, top + floorf(SEPARATOR_HEIGHT / 2), width - 2, 1, colours->separator);
            top += SEPARATOR_HEIGHT;
            continue;
        }
        if (item->kind == ITEM_HEADING) {
            ui_text(canvas, HEADING_INSET, top, HEADING_HEIGHT, item->title, colours->shortcut);
            top += HEADING_HEIGHT;
            continue;
        }
        bool selectable = item_selectable(item);
        bool highlighted = selectable && level->hover == i;
        if (highlighted) ui_fill(canvas, 1, top, width - 2, ROW_HEIGHT, colours->highlight);
        colour_t text = !selectable ? colours->disabled : highlighted ? colours->highlight_text : colours->text;
        if (item->kind == ITEM_ACTION && menu_state_checked(item->tag)) ui_mark(canvas, GLYPH_CHECK, CHECK_COLUMN / 2 + 1, top, ROW_HEIGHT, text);
        ui_text(canvas, CHECK_COLUMN, top, ROW_HEIGHT, item_title(item), text);
        if (item->shortcut[0]) {
            colour_t shortcut = !selectable ? colours->disabled : highlighted ? colours->highlight_text : colours->shortcut;
            ui_text(canvas, width - TEXT_RIGHT - ui_text_width(item->shortcut), top, ROW_HEIGHT, item->shortcut, shortcut);
        }
        if (item->kind == ITEM_SUBMENU) ui_mark(canvas, GLYPH_ARROW, width - ARROW_COLUMN / 2, top, ROW_HEIGHT, text);
        top += ROW_HEIGHT;
    }
}

static bool focus_is_ours(void) {
    SDL_Window *focus = SDL_GetKeyboardFocus();
    return focus && (focus == main_window || level_of_window(SDL_GetWindowID(focus)) >= 0);
}

static bool clicked_elsewhere(void) {
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "x11")) return false;
    float x, y;
    if (!SDL_GetGlobalMouseState(&x, &y)) return false;
    int window_x, window_y, width, height;
    SDL_GetWindowPosition(main_window, &window_x, &window_y);
    SDL_GetWindowSize(main_window, &width, &height);
    x -= (float)window_x;
    y -= (float)window_y;
    if (ui_inside(x, y, 0, 0, (float)width, (float)height)) return false;
    for (int level = 0; level < depth; level++) {
        float origin_x, origin_y;
        level_origin(level, &origin_x, &origin_y);
        if (ui_inside(x, y, origin_x, origin_y, levels[level].width, levels[level].height)) return false;
    }
    return true;
}

void menu_draw(SDL_Renderer *renderer) {
    if (!main_window) return;
    if (depth && (!focus_is_ours() || clicked_elsewhere())) close_levels(0);
    ui_prepare(main_window);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    draw_bar(renderer);
    for (int i = 0; i < depth; i++) {
        SDL_Renderer *popup = levels[i].canvas.renderer;
        SDL_SetRenderDrawBlendMode(popup, SDL_BLENDMODE_BLEND);
        draw_level(&levels[i]);
        SDL_RenderPresent(popup);
    }
}

int menu_modifiers(void) {
    return 0;
}

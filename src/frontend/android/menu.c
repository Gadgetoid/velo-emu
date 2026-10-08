#include "frontend/common/menu.h"

#include "frontend/android/keystrip.h"
#include "frontend/android/list.h"
#include "frontend/android/toast.h"
#include "frontend/common/menu_layout.h"
#include "frontend/common/menu_queue.h"
#include "frontend/common/menu_state.h"

#define PAGE_MAX (LIST_TAB_MAX - 1)

static SDL_Window *main_window;
static bool panel_open;
static int page;
static list_scroll_t scroll;

static int page_entries(int *starts) {
    int count = 0;
    for (int i = 0; i < MENU_ENTRY_COUNT && count < PAGE_MAX; i++) {
        if (MENU_ENTRIES[i].kind == MENU_ENTRY_MENU) starts[count++] = i;
    }
    return count;
}

static bool visible_item(const menu_entry_t *entry) {
    return entry->kind == MENU_ENTRY_ITEM && !menu_state_hidden(entry->tag);
}

static bool section_has_items(int from) {
    for (int i = from; i < MENU_ENTRY_COUNT; i++) {
        menu_entry_kind_t kind = MENU_ENTRIES[i].kind;
        if (kind == MENU_ENTRY_END || kind == MENU_ENTRY_SEPARATOR || kind == MENU_ENTRY_HEADING || kind == MENU_ENTRY_SUBMENU) return false;
        if (visible_item(&MENU_ENTRIES[i])) return true;
    }
    return false;
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
            if (section_has_items(i + 1)) list_add_row(list, ROW_HEADING, 0, entry->title, false, false);
        } else if (entry->kind == MENU_ENTRY_SEPARATOR) {
            if (list->row_count && list->rows[list->row_count - 1].kind != ROW_SEPARATOR) list_add_row(list, ROW_SEPARATOR, 0, NULL, false, false);
        } else if (visible_item(entry)) {
            int tag = entry->tag;
            list_add_row(list, ROW_ITEM, tag, menu_state_title(tag, entry->title), menu_state_checked(tag), !menu_state_enabled(tag));
        }
    }
    while (list->row_count && list->rows[list->row_count - 1].kind == ROW_SEPARATOR) list->row_count--;
}

static void open_panel(bool open) {
    panel_open = open;
    list_reset(&scroll);
    if (open) {
        keystrip_release();
        if (SDL_ScreenKeyboardShown(main_window)) SDL_StopTextInput(main_window);
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
    tap_t tap = list_event(&scroll, &list, main_window, event);
    if (!tap.tapped) return true;
    if (tap.tab == list.tab_count - 1) {
        open_panel(false);
    } else if (tap.tab >= 0 && tap.tab != page) {
        page = tap.tab;
        list_reset(&scroll);
    } else if (tap.tag >= 0) {
        menu_queue_push(tap.tag);
        open_panel(false);
    }
    return true;
}

void menu_install(SDL_Window *window) {
    main_window = window;
    keystrip_install(window);
}

int menu_bar_height(void) {
    int left, top, right, bottom;
    keystrip_insets(&left, &top, &right, &bottom);
    return top;
}

void menu_insets(int *left, int *top, int *right, int *bottom) {
    keystrip_insets(left, top, right, bottom);
}

bool menu_event(const SDL_Event *event) {
    if (panel_open) return panel_event(event);
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key != SDLK_AC_BACK) return false;
        open_panel(true);
        return true;
    case SDL_EVENT_KEY_UP:
        return event->key.key == SDLK_AC_BACK;
    default: {
        bool open_menu = false;
        bool handled = keystrip_event(event, &open_menu);
        if (open_menu) open_panel(true);
        return handled;
    }
    }
}

bool menu_active(void) {
    return panel_open;
}

void menu_draw(SDL_Renderer *renderer) {
    keystrip_draw(renderer);
    if (panel_open) {
        list_t list;
        panel_list(&list);
        list_draw(&scroll, &list, main_window, renderer);
    }
    toast_draw(main_window, renderer);
}

void menu_ensure(void) {
}

int menu_modifiers(void) {
    return 0;
}

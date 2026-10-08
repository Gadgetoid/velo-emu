#include "frontend/common/menu_state.h"

#include <SDL3/SDL.h>

#include "frontend/common/menu.h"

#define TITLE_MAX 96

static bool disabled[MENU_COUNT], checked[MENU_COUNT], hidden[MENU_COUNT];
static char titles[MENU_COUNT][TITLE_MAX];

static bool valid(int item) {
    return item >= 0 && item < MENU_COUNT;
}

void menu_set_enabled(int item, bool on) {
    if (valid(item)) disabled[item] = !on;
}

void menu_set_checked(int item, bool on) {
    if (valid(item)) checked[item] = on;
}

void menu_set_hidden(int item, bool on) {
    if (valid(item)) hidden[item] = on;
}

void menu_set_title(int item, const char *title) {
    if (valid(item)) SDL_strlcpy(titles[item], title, sizeof titles[item]);
}

bool menu_state_enabled(int item) {
    return valid(item) && !disabled[item];
}

bool menu_state_checked(int item) {
    return valid(item) && checked[item];
}

bool menu_state_hidden(int item) {
    return valid(item) && hidden[item];
}

const char *menu_state_title(int item, const char *fallback) {
    return valid(item) && titles[item][0] ? titles[item] : fallback;
}

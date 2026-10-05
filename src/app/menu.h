#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

enum {
    MENU_POWER,
    MENU_PAUSE,
    MENU_SOFT_RESET,
    MENU_MACHINE_FIRST,
    MENU_MACHINE_LAST = MENU_MACHINE_FIRST + 15,
    MENU_NEW_MACHINE,
    MENU_MANAGE_MACHINES,
    MENU_SAVE_STATE,
    MENU_LOAD_STATE,
    MENU_BACKLIGHT,
    MENU_SOUND,
    MENU_INSERT_CARD,
    MENU_EJECT_CARD,
    MENU_INSERT_DISK,
    MENU_NEW_DISK,
    MENU_EJECT_DISK,
    MENU_SERIAL_NETWORK,
    MENU_SERIAL_PTY,
    MENU_SERIAL_OFF,
    MENU_SERIAL_PORT_FIRST,
    MENU_SERIAL_PORT_LAST = MENU_SERIAL_PORT_FIRST + 15,
    MENU_SHOW_STATE,
    MENU_SPEED_1,
    MENU_SPEED_2,
    MENU_SPEED_4,
    MENU_SPEED_8,
    MENU_SEND_FILES,
    MENU_FETCH_DOCUMENTS,
    MENU_SHARED_FOLDER,
    MENU_SYNC_NOW,
    MENU_STOP_SHARING,
    MENU_SET_PROXY,
    MENU_BAUD_19200,
    MENU_BAUD_38400,
    MENU_BAUD_57600,
    MENU_BAUD_115200,
    MENU_PASTE,
    MENU_CONNECT_AT_LAUNCH,
    MENU_SAVE_SNAPSHOT,
    MENU_LOAD_SNAPSHOT,
    MENU_COPY_SCREEN,
    MENU_SAVE_SCREENSHOT,
    MENU_SCALE_50,
    MENU_SCALE_75,
    MENU_SCALE_100,
    MENU_SCALE_150,
    MENU_SCALE_200,
    MENU_ZOOM_IN,
    MENU_ZOOM_OUT,
    MENU_FULL_SCREEN,
    MENU_DISPLAY_SIMULATED,
    MENU_DISPLAY_SHARP,
    MENU_SHOW_DEBUG_OUTPUT,
    MENU_QUIT,
    MENU_IMPORT,
    MENU_COUNT,
};

void menu_install(SDL_Window *window);
int  menu_bar_height(void);
void menu_insets(int *left, int *top, int *right, int *bottom);
bool menu_event(const SDL_Event *event);
bool menu_active(void);
void menu_draw(SDL_Renderer *renderer);
void menu_ensure(void);
int  menu_poll(void);
void menu_set_checked(int item, bool checked);
void menu_set_enabled(int item, bool enabled);
void menu_set_title(int item, const char *title);
void menu_set_hidden(int item, bool hidden);

enum { MENU_MOD_SHIFT = 1, MENU_MOD_CONTROL = 2, MENU_MOD_ALT = 4, MENU_MOD_KNOWN = 8 };
int  menu_modifiers(void);

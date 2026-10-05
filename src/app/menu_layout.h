#pragma once
#include "app/menu.h"

#ifdef __APPLE__
#define MENU_HOST             "Mac"
#define MENU_SCREENSHOT_PLACE "Desktop"
#else
#define MENU_HOST             "Computer"
#define MENU_SCREENSHOT_PLACE "Pictures"
#endif

typedef enum {
    MENU_ENTRY_MENU,
    MENU_ENTRY_SUBMENU,
    MENU_ENTRY_END,
    MENU_ENTRY_ITEM,
    MENU_ENTRY_SEPARATOR,
    MENU_ENTRY_HEADING,
} menu_entry_kind_t;

enum {
    MENU_KEY_PRIMARY = 1,
    MENU_KEY_SHIFT = 2,
    MENU_KEY_CONTROL = 4,
};

typedef struct {
    menu_entry_kind_t kind;
    int tag;
    const char *title;
    char key;
    int modifiers;
} menu_entry_t;

#define ELLIPSIS "\xe2\x80\xa6"

static const menu_entry_t MENU_ENTRIES[] = {
    { MENU_ENTRY_MENU, 0, "Machine", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_POWER, "Power Button", 'p', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_ITEM, MENU_BACKLIGHT, "Backlight", 'b', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SOFT_RESET, "Soft Reset", 'r', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_HEADING, 0, "Machines", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 0, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 1, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 2, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 3, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 4, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 5, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 6, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 7, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 8, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 9, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 10, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 11, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 12, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 13, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 14, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MACHINE_FIRST + 15, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_NEW_MACHINE, "New Machine" ELLIPSIS, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_MANAGE_MACHINES, "Manage Machines" ELLIPSIS, 0, 0 },
#ifdef __ANDROID__
    { MENU_ENTRY_ITEM, MENU_IMPORT, "Import ROMs and Cards" ELLIPSIS, 0, 0 },
#endif
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_PAUSE, "Pause", 'p', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SUBMENU, 0, "CPU Speed", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_1, "1x (original)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_2, "2x", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_4, "4x", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SPEED_8, "8x", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SHOW_DEBUG_OUTPUT, "Show Debug Output", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_GDB_SERVER, "GDB Server", 0, 0 },
#ifndef __APPLE__
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_QUIT, "Quit", 'q', MENU_KEY_PRIMARY },
#endif
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "State", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_STATE, "Save State", 's', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_LOAD_STATE, "Load State", 'l', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_SNAPSHOT, "Save Snapshot" ELLIPSIS, 's', MENU_KEY_PRIMARY | MENU_KEY_CONTROL },
    { MENU_ENTRY_ITEM, MENU_LOAD_SNAPSHOT, "Load Snapshot" ELLIPSIS, 'l', MENU_KEY_PRIMARY | MENU_KEY_CONTROL },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SHOW_STATE, "Show State Folder", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Edit", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_COPY_SCREEN, "Copy Screen", 'c', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_PASTE, "Paste as Typing", 'v', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SAVE_SCREENSHOT, "Save Screenshot to " MENU_SCREENSHOT_PLACE, 's', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "View", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_50, "50%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_75, "75%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_100, "Actual Size", '0', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_SCALE_150, "150%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SCALE_200, "200%", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_ZOOM_IN, "Zoom In", '=', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_ZOOM_OUT, "Zoom Out", '-', MENU_KEY_PRIMARY },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_DISPLAY_SIMULATED, "Simulated LCD", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_DISPLAY_SHARP, "Sharp Pixels", 0, 0 },
#ifdef __ANDROID__
    { MENU_ENTRY_ITEM, MENU_FULL_BRIGHTNESS, "Full Brightness with Backlight", 0, 0 },
#endif
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_FULL_SCREEN, "Full Screen", 'f', MENU_KEY_PRIMARY | MENU_KEY_CONTROL },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "Devices", 0, 0 },
    { MENU_ENTRY_HEADING, 0, "PC Card", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_INSERT_CARD, "Insert Card Image" ELLIPSIS, 'o', MENU_KEY_PRIMARY },
    { MENU_ENTRY_ITEM, MENU_EJECT_CARD, "Eject Card", 'e', MENU_KEY_PRIMARY },
    { MENU_ENTRY_HEADING, 0, "Paravirtual Disk", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_INSERT_DISK, "Insert Disk Image" ELLIPSIS, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_NEW_DISK, "New Disk Image" ELLIPSIS, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_EJECT_DISK, "Eject Disk", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_HEADING, 0, "Serial Port", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_OFF, "Not Connected", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_NETWORK, "Network (PPP)", 'n', MENU_KEY_PRIMARY | MENU_KEY_SHIFT },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PTY, "Pseudo-terminal", 0, 0 },
    { MENU_ENTRY_SUBMENU, 0, "Host Serial Port", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 0, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 1, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 2, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 3, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 4, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 5, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 6, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 7, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 8, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 9, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 10, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 11, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 12, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 13, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 14, "", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SERIAL_PORT_FIRST + 15, "", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_CONNECT_AT_LAUNCH, "Connect Network at Launch", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SOUND, "Sound", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },

    { MENU_ENTRY_MENU, 0, "PC Link", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SEND_FILES, "Send Files to Velo" ELLIPSIS, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_FETCH_DOCUMENTS, "Copy My Documents to " MENU_HOST ELLIPSIS, 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SHARED_FOLDER, "Shared Folder" ELLIPSIS, 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SYNC_NOW, "Sync Shared Folder Now", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_STOP_SHARING, "Stop Sharing Folder", 0, 0 },
    { MENU_ENTRY_SEPARATOR, 0, NULL, 0, 0 },
    { MENU_ENTRY_HEADING, 0, "Velo Settings", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_SET_PROXY, "Set Up Pocket IE Proxy", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_NETWORK_RAPI, "RAPI over the Network", 0, 0 },
    { MENU_ENTRY_SUBMENU, 0, "Connection Speed", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_19200, "19200 (original)", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_38400, "38400", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_57600, "57600", 0, 0 },
    { MENU_ENTRY_ITEM, MENU_BAUD_115200, "115200", 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
    { MENU_ENTRY_END, 0, NULL, 0, 0 },
};

static const int MENU_ENTRY_COUNT = sizeof MENU_ENTRIES / sizeof MENU_ENTRIES[0];

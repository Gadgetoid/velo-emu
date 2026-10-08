#include "frontend/common/dialog.h"

#ifdef __APPLE__
#define CLOCK_SOURCE "this Mac"
#elif defined(__ANDROID__)
#define CLOCK_SOURCE "this phone"
#else
#define CLOCK_SOURCE "this computer"
#endif

const uint32_t DIALOG_MEMORY_SIZES[] = { 4, 8, 16, 20, 32 };

const int DIALOG_MEMORY_COUNT = (int)(sizeof DIALOG_MEMORY_SIZES / sizeof DIALOG_MEMORY_SIZES[0]);

const char *const DIALOG_MEMORY_LABELS[] = {
    "4 MB (original)", "8 MB", "16 MB", "20 MB (4 MB + 16 MB DRAM card)", "32 MB (16 MB + 16 MB DRAM card)",
};

const char *const DIALOG_NEW_MACHINE_MESSAGE =
    "The screen, memory and clock settings are fixed for the life of the machine. The ROM decides which screen sizes are available.";

const char *const DIALOG_MANAGE_MESSAGE =
    "Reset sets a machine back to its factory state. Delete removes it and its saved state. Both put a backup in Snapshots/Backups first. The running machine can be reset but not deleted.";

const char *const DIALOG_CLOCK_LABEL = "Set the clock from " CLOCK_SOURCE " at the first boot";

const char *const DIALOG_ROM_PROMPT = "Choose a Velo 1 ROM: a CE 1.0 nk.bin, a CE 2.0 card ROM or merged image, or a B000FF image.";

const char *const DIALOG_NOT_A_ROM = "Not a Velo ROM";

const char *const DIALOG_NO_ROM_HINT = "Put a Velo ROM in the roms folder, or choose Other ROM File" "\xe2\x80\xa6" ".";

static const char *const SCREEN_LABELS[] = { "480 x 240 (original)", "640 x 240", "640 x 480", "800 x 600" };

const char *dialog_screen_label(int preset) {
    return preset >= 0 && preset < (int)(sizeof SCREEN_LABELS / sizeof SCREEN_LABELS[0]) ? SCREEN_LABELS[preset] : NULL;
}

bool dialog_rom_allows_screen(const dialog_rom_t *rom, int preset) {
    return preset >= 0 && preset < 32 && (rom->screens & (1u << preset));
}

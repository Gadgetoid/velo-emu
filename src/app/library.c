#include "app/library.h"

#include <SDL3/SDL.h>

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "app/host.h"
#include "app/paths.h"
#include "frontend/android/android.h"
#include "util/fat.h"
#include "util/file.h"

#define IMPORT_MAX 64

static void rom_folder(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/roms", base);
    SDL_CreateDirectory(path);
}

#define CARD_MIN_BYTES  (1024 * 1024)

void library_find_roms(rom_set_t *roms) {
    char folder[1100];
    rom_folder(folder, sizeof folder);
    rom_catalog_find(roms, folder);
}

static void cards_folder(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/cards", base);
    SDL_CreateDirectory(path);
}

static int library_score(const char *path, int system) {
    bool ce1 = fat_root_has_folder(path, "VELOLIB"), ce2 = fat_root_has_folder(path, "VELOLIB2");
    bool wanted = system == 2 ? ce2 : ce1, other = system == 2 ? ce1 : ce2;
    return wanted ? (other ? 1 : 2) : 0;
}

void library_insert_card(machine_t *machine) {
    int system = machine_rom_system(machine);
    char folder[1100], best[1200] = "";
    cards_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return;
    int best_score = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.') continue;
        char path[1200];
        if (snprintf(path, sizeof path, "%s/%s", folder, entry->d_name) >= (int)sizeof path) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < CARD_MIN_BYTES) continue;
        int score = library_score(path, system);
        if (score > best_score) {
            best_score = score;
            memcpy(best, path, sizeof best);
        }
    }
    closedir(dir);
    if (best[0] && machine_insert_card(machine, best)) fprintf(stderr, "inserted the software library %s\n", file_leaf_name(best));
}

void library_show_no_roms(void) {
    char folder[1100], message[1400];
    rom_folder(folder, sizeof folder);
    snprintf(message, sizeof message, "Put a Velo 1 ROM in %s: the CE 1.0 nk.bin, the merged CE 2.0 image, or both.", folder);
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Show ROM Folder" },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_INFORMATION, NULL, "No Velo ROM found", message, 2, buttons, NULL };
    int chosen = 0;
    if (SDL_ShowMessageBox(&dialog, &chosen) && chosen == 1) host_open_path(folder);
}

#ifdef __ANDROID__
typedef struct {
    SDL_AtomicInt done;
    int count;
    char uris[IMPORT_MAX][1024];
} import_pick_t;

static void import_picked(void *userdata, const char *const *files, int filter) {
    (void)filter;
    import_pick_t *pick = userdata;
    pick->count = 0;
    while (files && files[pick->count] && pick->count < IMPORT_MAX) {
        snprintf(pick->uris[pick->count], sizeof pick->uris[0], "%s", files[pick->count]);
        pick->count++;
    }
    SDL_SetAtomicInt(&pick->done, 1);
}

int library_import_files(int *cards) {
    import_pick_t pick = { 0 };
    pick.count = 0;
    SDL_SetAtomicInt(&pick.done, 0);
    SDL_ShowOpenFileDialog(import_picked, &pick, NULL, NULL, 0, NULL, true);
    while (!SDL_GetAtomicInt(&pick.done)) {
        SDL_Event event;
        if (SDL_WaitEventTimeout(&event, 100) && event.type == SDL_EVENT_QUIT) SDL_PushEvent(&event);
    }
    char roms[1100], card_folder[1100];
    rom_folder(roms, sizeof roms);
    cards_folder(card_folder, sizeof card_folder);
    int rom_count = 0;
    *cards = 0;
    for (int i = 0; i < pick.count; i++) {
        char path[1200];
        if (!android_import(pick.uris[i], roms, path, sizeof path)) continue;
        struct stat info;
        if (rom_catalog_probe(path, NULL)) {
            rom_count++;
            continue;
        }
        char card[1200];
        snprintf(card, sizeof card, "%s/%s", card_folder, file_leaf_name(path));
        if (stat(path, &info) == 0 && info.st_size >= CARD_MIN_BYTES && info.st_size % 512 == 0 && rename(path, card) == 0) (*cards)++;
        else remove(path);
    }
    return rom_count;
}

bool library_first_run_import(void) {
    SDL_Init(SDL_INIT_VIDEO);
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose Files" },
    };
    const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_INFORMATION, NULL, "Import ROMs and Cards",
                                        "Choose your Velo 1 ROMs: the CE 1.0 nk.bin, a merged CE 2.0 image, or both. Card images, such as the Velo Software Library, can be chosen at the same time.",
                                        2, buttons, NULL };
    int chosen = 0;
    if (!SDL_ShowMessageBox(&dialog, &chosen) || chosen != 1) return false;
    int cards;
    library_import_files(&cards);
    return true;
}
#endif

void library_machines_folder(char *path, size_t size) {
    char base[1024];
    app_data_folder(base, sizeof base);
    snprintf(path, size, "%s/machines", base);
    SDL_CreateDirectory(path);
}

int library_list_roms(dialog_rom_t *roms, int max) {
    char folder[1100];
    rom_folder(folder, sizeof folder);
    DIR *dir = opendir(folder);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (entry->d_name[0] == '.') continue;
        dialog_rom_t *rom = &roms[count];
        if (snprintf(rom->path, sizeof rom->path, "%s/%s", folder, entry->d_name) >= (int)sizeof rom->path) continue;
        rom->screens = rom_catalog_label(rom->path, rom->label, sizeof rom->label);
        if (rom->screens) count++;
    }
    closedir(dir);
    return count;
}

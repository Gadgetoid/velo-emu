#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app/dialog.h"
#include "app/rom_catalog.h"
#include "core/machine.h"

void library_find_roms(rom_set_t *roms);
void library_machines_folder(char *path, size_t size);
int  library_list_roms(dialog_rom_t *roms, int max);
void library_insert_card(machine_t *machine);
void library_show_no_roms(void);
#ifdef __ANDROID__
int  library_import_files(int *cards);
bool library_first_run_import(void);
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>

#define ROM_CATALOG_SYSTEMS 3

typedef struct {
    char path[ROM_CATALOG_SYSTEMS][1024];
    size_t size[ROM_CATALOG_SYSTEMS];
    int rank[ROM_CATALOG_SYSTEMS];
} rom_set_t;

void     rom_catalog_find(rom_set_t *roms, const char *folder);
int      rom_catalog_probe(const char *path, uint32_t *screens);
uint32_t rom_catalog_label(const char *path, char *label, size_t label_size);
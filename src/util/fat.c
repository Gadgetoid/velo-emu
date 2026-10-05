#include "util/fat.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SECTOR_BYTES     512
#define ENTRY_BYTES      32
#define ATTRIBUTE_FOLDER 0x10

static uint16_t le16(const uint8_t *at) { return (uint16_t)(at[0] | at[1] << 8); }
static uint32_t le32(const uint8_t *at) { return (uint32_t)at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16 | (uint32_t)at[3] << 24; }

static bool read_sector(FILE *file, uint32_t lba, uint8_t *sector) {
    return fseek(file, (long)lba * SECTOR_BYTES, SEEK_SET) == 0 && fread(sector, 1, SECTOR_BYTES, file) == SECTOR_BYTES;
}

static bool boot_sector(const uint8_t *sector) {
    return (sector[0] == 0xEB || sector[0] == 0xE9) && le16(sector + 0x0B) == SECTOR_BYTES && sector[0x0D] && sector[0x10];
}

bool fat_root_has_folder(const char *image, const char *name) {
    char wanted[11];
    memset(wanted, ' ', sizeof wanted);
    for (size_t i = 0; name[i] && i < sizeof wanted; i++) wanted[i] = (char)toupper((unsigned char)name[i]);
    FILE *file = fopen(image, "rb");
    if (!file) return false;
    uint8_t sector[SECTOR_BYTES];
    bool found = false;
    uint32_t start = 0;
    if (!read_sector(file, 0, sector) || sector[0x1FE] != 0x55 || sector[0x1FF] != 0xAA) goto done;
    if (!boot_sector(sector)) {
        start = le32(sector + 0x1C6);
        if (!start || !read_sector(file, start, sector) || !boot_sector(sector)) goto done;
    }
    uint32_t root_entries = le16(sector + 0x11), sectors_per_fat = le16(sector + 0x16);
    if (!root_entries || !sectors_per_fat) goto done;
    uint32_t root = start + le16(sector + 0x0E) + sector[0x10] * sectors_per_fat;
    uint32_t root_sectors = (root_entries * ENTRY_BYTES + SECTOR_BYTES - 1) / SECTOR_BYTES;
    for (uint32_t i = 0; i < root_sectors && !found; i++) {
        if (!read_sector(file, root + i, sector)) break;
        for (int entry = 0; entry < SECTOR_BYTES / ENTRY_BYTES; entry++) {
            const uint8_t *at = sector + entry * ENTRY_BYTES;
            if (at[0] == 0) goto done;
            if (at[0] == 0xE5 || !(at[11] & ATTRIBUTE_FOLDER) || (at[11] & 0x0F) == 0x0F) continue;
            if (!memcmp(at, wanted, sizeof wanted)) {
                found = true;
                break;
            }
        }
    }
done:
    fclose(file);
    return found;
}

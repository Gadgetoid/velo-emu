#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define VDISK_PA          0x10800000u
#define VDISK_WINDOW      0x8000u
#define VDISK_SECTOR      512u
#define VDISK_BUFFER      0x1000u
#define VDISK_MAX_SECTORS 32u
#define VDISK_MAGIC       0x4B534456u

enum {
    VDISK_REG_MAGIC = 0x00,
    VDISK_REG_VERSION = 0x04,
    VDISK_REG_SECTORS = 0x08,
    VDISK_REG_FLAGS = 0x0C,
    VDISK_REG_LBA = 0x10,
    VDISK_REG_COUNT = 0x14,
    VDISK_REG_COMMAND = 0x18,
    VDISK_REG_STATUS = 0x1C,
    VDISK_REG_CHANGES = 0x20,
};

enum { VDISK_COMMAND_READ = 1, VDISK_COMMAND_WRITE = 2 };
enum { VDISK_STATUS_OK, VDISK_STATUS_RANGE, VDISK_STATUS_IO, VDISK_STATUS_NO_MEDIA, VDISK_STATUS_READ_ONLY };
enum { VDISK_FLAG_READ_ONLY = 1 };

typedef struct {
    uint32_t lba, count, status, changes;
    uint8_t buffer[VDISK_MAX_SECTORS * VDISK_SECTOR];
} vdisk_t;

typedef struct {
    vdisk_t *state;
    FILE    *image;
    uint32_t sectors;
    bool read_only;
} vdisk_port_t;

bool     vdisk_insert(vdisk_port_t *port, FILE *image, bool read_only);
void     vdisk_eject(vdisk_port_t *port);
uint32_t vdisk_read(vdisk_port_t *port, uint32_t offset, int size);
void     vdisk_write(vdisk_port_t *port, uint32_t offset, int size, uint32_t value);

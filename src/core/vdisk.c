#include "core/vdisk.h"

#include <string.h>

bool vdisk_insert(vdisk_port_t *port, FILE *image, bool read_only) {
    if (fseek(image, 0, SEEK_END) != 0) return false;
    long length = ftell(image);
    if (length < (long)VDISK_SECTOR) return false;
    if (port->image) fclose(port->image);
    port->image = image;
    port->sectors = (uint32_t)(length / VDISK_SECTOR);
    port->read_only = read_only;
    port->state->changes++;
    return true;
}

void vdisk_eject(vdisk_port_t *port) {
    if (port->image) fclose(port->image);
    port->image = NULL;
    port->sectors = 0;
    port->state->changes++;
}

static uint32_t transfer(vdisk_port_t *port, uint32_t command) {
    vdisk_t *disk = port->state;
    if (!port->image) return VDISK_STATUS_NO_MEDIA;
    if (disk->count == 0 || disk->count > VDISK_MAX_SECTORS || disk->lba >= port->sectors || disk->count > port->sectors - disk->lba) return VDISK_STATUS_RANGE;
    if (command == VDISK_COMMAND_WRITE && port->read_only) return VDISK_STATUS_READ_ONLY;
    size_t bytes = (size_t)disk->count * VDISK_SECTOR;
    if (fseek(port->image, (long)disk->lba * VDISK_SECTOR, SEEK_SET) != 0) return VDISK_STATUS_IO;
    if (command == VDISK_COMMAND_READ) return fread(disk->buffer, 1, bytes, port->image) == bytes ? VDISK_STATUS_OK : VDISK_STATUS_IO;
    if (fwrite(disk->buffer, 1, bytes, port->image) != bytes || fflush(port->image) != 0) return VDISK_STATUS_IO;
    return VDISK_STATUS_OK;
}

static uint32_t read_buffer(const uint8_t *at, int size) {
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value |= (uint32_t)at[i] << (8 * i);
    return value;
}

uint32_t vdisk_read(vdisk_port_t *port, uint32_t offset, int size) {
    vdisk_t *disk = port->state;
    if (offset >= VDISK_BUFFER) {
        uint32_t at = offset - VDISK_BUFFER;
        return at + (uint32_t)size <= sizeof disk->buffer ? read_buffer(disk->buffer + at, size) : 0;
    }
    switch (offset & ~3u) {
    case VDISK_REG_MAGIC: return VDISK_MAGIC;
    case VDISK_REG_VERSION: return 1;
    case VDISK_REG_SECTORS: return port->sectors;
    case VDISK_REG_FLAGS: return port->read_only ? VDISK_FLAG_READ_ONLY : 0;
    case VDISK_REG_LBA: return disk->lba;
    case VDISK_REG_COUNT: return disk->count;
    case VDISK_REG_STATUS: return disk->status;
    case VDISK_REG_CHANGES: return disk->changes;
    }
    return 0;
}

void vdisk_write(vdisk_port_t *port, uint32_t offset, int size, uint32_t value) {
    vdisk_t *disk = port->state;
    if (offset >= VDISK_BUFFER) {
        uint32_t at = offset - VDISK_BUFFER;
        if (at + (uint32_t)size > sizeof disk->buffer) return;
        for (int i = 0; i < size; i++) disk->buffer[at + i] = (uint8_t)(value >> (8 * i));
        return;
    }
    switch (offset & ~3u) {
    case VDISK_REG_LBA: disk->lba = value; break;
    case VDISK_REG_COUNT: disk->count = value; break;
    case VDISK_REG_COMMAND:
        if (value == VDISK_COMMAND_READ || value == VDISK_COMMAND_WRITE) disk->status = transfer(port, value);
        break;
    }
}

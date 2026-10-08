#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PCCARD_CTRL_WINDOW_PA  0x08000000u
#define PCCARD_CTRL_WINDOW_END 0x10000000u
#define PCCARD_MEM_WINDOW_PA   0x64000000u
#define PCCARD_MEM_WINDOW_END  0x6C000000u
#define PCCARD_IT8368_SIZE     0x24u

typedef struct {
    uint16_t gpio_dataout, gpio_dir;
    uint16_t gpio_posinten, gpio_neginten;
    uint16_t gpio_posintstat, gpio_negintstat;
    uint16_t mfio_posintstat, mfio_negintstat;
    uint16_t mfio_dataout, mfio_dir, mfio_sel;
    uint16_t ctrl;
    uint16_t prev_datain;
    bool int_asserted;
    bool reset_asserted;

    bool inserted;
    bool powered;
    bool card_irq;

    uint8_t feature, error, sector_count, sector_number, cylinder_low, cylinder_high, drive_head;
    uint8_t status, device_control, cor;
    uint8_t buffer[512];
    uint32_t buffer_position;
    uint32_t sectors_left;
    bool writing;
    uint64_t total_sectors;
} pccard_t;

typedef void (*pccard_int_fn)(void *context, bool asserted);

typedef struct {
    pccard_t     *state;
    FILE         *image;
    pccard_int_fn int_changed;
    void         *context;
} pccard_socket_t;

void     pccard_reset(pccard_socket_t *socket);
bool     pccard_insert(pccard_socket_t *socket, FILE *image);
void     pccard_eject(pccard_socket_t *socket);
void     pccard_rebind(pccard_socket_t *socket, FILE *image);
void     pccard_sanitize(pccard_t *card);

uint16_t pccard_it8368_read(pccard_socket_t *socket, uint32_t offset);
void     pccard_it8368_write(pccard_socket_t *socket, uint32_t offset, uint16_t value);

uint32_t pccard_read(pccard_socket_t *socket, uint32_t pa, int size);
void     pccard_write(pccard_socket_t *socket, uint32_t pa, int size, uint32_t value);

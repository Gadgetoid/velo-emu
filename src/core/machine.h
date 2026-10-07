#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "core/key_text.h"
#include "core/mailbox.h"
#include "core/mips.h"
#include "core/screen.h"

#define MACHINE_CLOCK_HZ      36864000u
#define MACHINE_WATCH_MAX     4

typedef struct machine machine_t;

typedef struct {
    uint32_t base, stride, width, height, bpp;
    uint8_t  shades[16];
} machine_lcd_t;

typedef void (*machine_log_fn)(const char *message);
typedef void (*machine_debug_fn)(void *context, const char *line);

machine_t *machine_create(const uint8_t *rom, size_t rom_size, char *error, size_t error_size);
machine_t *machine_create_in_place(const uint8_t *rom, size_t rom_size, uint8_t *dram, uint32_t dram_size, char *error, size_t error_size);
void       machine_destroy(machine_t *machine);
void       machine_set_log(machine_t *machine, machine_log_fn log);
void       machine_run(machine_t *machine, uint64_t cycles);
mips_cpu_t *machine_cpu(machine_t *machine);
mailbox_t *machine_mailbox(machine_t *machine);
bool       machine_read_physical(machine_t *machine, uint32_t pa, uint8_t *data, uint32_t length);
bool       machine_write_physical(machine_t *machine, uint32_t pa, const uint8_t *data, uint32_t length);
uint64_t   machine_cycles(machine_t *machine);
uint32_t   machine_pc(machine_t *machine);

bool machine_lcd_enabled(machine_t *machine);
bool machine_backlight(machine_t *machine);
void machine_backlight_button(machine_t *machine, bool down);
bool machine_screen(machine_t *machine, uint8_t *levels);
bool machine_lcd_format(machine_t *machine, machine_lcd_t *lcd);
screen_size_t machine_screen_size(machine_t *machine);
screen_size_t machine_screen_next(machine_t *machine);
bool          machine_screen_supported(machine_t *machine, screen_size_t size);
bool          machine_set_screen(machine_t *machine, screen_size_t size);

void machine_key(machine_t *machine, uint8_t scancode, bool up);
void machine_touch(machine_t *machine, bool down, int x, int y);
void machine_power_button(machine_t *machine, bool down);
bool machine_suspended(machine_t *machine);

size_t machine_audio(machine_t *machine, int16_t *samples, size_t max, uint32_t *rate);

bool machine_insert_card(machine_t *machine, const char *path);
bool machine_insert_card_file(machine_t *machine, FILE *image, const char *name);
void machine_eject_card(machine_t *machine);
bool machine_card_inserted(machine_t *machine);

bool machine_insert_disk(machine_t *machine, const char *path, bool read_only);
void machine_eject_disk(machine_t *machine);
bool machine_disk_inserted(machine_t *machine);

void   machine_serial_connect(machine_t *machine, bool connected);
bool   machine_serial_connected(machine_t *machine);
void   machine_set_serial_tag(machine_t *machine, uint32_t tag);
uint32_t machine_serial_tag(machine_t *machine);
void   machine_serial_send(machine_t *machine, const uint8_t *data, size_t length);
size_t machine_serial_take(machine_t *machine, uint8_t *out, size_t max);
uint32_t machine_serial_baud(machine_t *machine);
bool   machine_serial_dtr(machine_t *machine);
size_t machine_serial_space(machine_t *machine);

void machine_reset(machine_t *machine);
void machine_soft_reset(machine_t *machine);
bool machine_watch_pc(machine_t *machine, uint32_t va);
void     machine_set_memory(machine_t *machine, uint32_t megabytes);
uint32_t machine_memory(machine_t *machine);
uint32_t machine_memory_next(machine_t *machine);
void     machine_set_speed(machine_t *machine, uint32_t multiplier);
void     machine_set_optimisations(machine_t *machine, bool optimisations);
bool     machine_optimisations(machine_t *machine);
uint32_t machine_speed(machine_t *machine);
bool machine_save(machine_t *machine, const char *path, int64_t host_time);
uint64_t machine_rom_hash(machine_t *machine);
int      machine_rom_system(machine_t *machine);
key_layout_t machine_key_layout(machine_t *machine);
bool machine_state_matches(machine_t *machine, const char *path);
bool machine_load(machine_t *machine, const char *path, int64_t *host_time);
void machine_advance_clock(machine_t *machine, int64_t seconds);
void machine_set_host_clock(machine_t *machine, bool enabled);
void machine_set_debug_output(machine_t *machine, machine_debug_fn sink, void *context);

void machine_dump_state(machine_t *machine);

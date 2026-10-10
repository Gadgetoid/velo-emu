#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NE2000_FRAME_MAX 1514
#define NE2000_FRAMES    16
#define NE2000_MEMORY    0x8000u

typedef struct {
    bool inserted;
    uint8_t mac[6];
    uint8_t cor;
    uint8_t command, page_start, page_stop, boundary, transmit_page, interrupt_status, interrupt_mask;
    uint8_t receive_config, transmit_config, data_config, current_page, transmit_status, receive_status;
    uint8_t physical[6], multicast[8];
    uint16_t transmit_count, remote_start, remote_count, remote_address;
    uint8_t memory[NE2000_MEMORY];
    uint8_t sent[NE2000_FRAMES][NE2000_FRAME_MAX];
    uint16_t sent_length[NE2000_FRAMES];
    uint32_t sent_head, sent_count;
} ne2000_t;

void     ne2000_insert(ne2000_t *card, const uint8_t mac[6]);
void     ne2000_eject(ne2000_t *card);
void     ne2000_reset(ne2000_t *card);
void     ne2000_power_on(ne2000_t *card);
void     ne2000_sanitize(ne2000_t *card);
bool     ne2000_interrupt(const ne2000_t *card);
bool     ne2000_io_mode(const ne2000_t *card);
uint32_t ne2000_attribute_read(ne2000_t *card, uint32_t offset, int size);
void     ne2000_attribute_write(ne2000_t *card, uint32_t offset, int size, uint32_t value);
uint32_t ne2000_io_read(ne2000_t *card, uint32_t offset, int size);
void     ne2000_io_write(ne2000_t *card, uint32_t offset, int size, uint32_t value);
bool     ne2000_receive(ne2000_t *card, const uint8_t *frame, size_t length);
size_t   ne2000_take_sent(ne2000_t *card, uint8_t *frame, size_t max);

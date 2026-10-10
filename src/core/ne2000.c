#include "core/ne2000.h"

#include <string.h>

#define CR_STOP        0x01u
#define CR_START       0x02u
#define CR_TRANSMIT    0x04u
#define CR_DMA_MASK    0x38u
#define CR_DMA_READ    0x08u
#define CR_DMA_WRITE   0x10u
#define CR_DMA_ABORT   0x20u
#define CR_PAGE_SHIFT  6

#define ISR_RECEIVED     0x01u
#define ISR_TRANSMITTED  0x02u
#define ISR_OVERWRITE    0x10u
#define ISR_DMA_COMPLETE 0x40u
#define ISR_RESET        0x80u
#define ISR_SOURCES      0x7Fu

#define RCR_BROADCAST    0x04u
#define RCR_MULTICAST    0x08u
#define RCR_PROMISCUOUS  0x10u
#define DCR_WORD         0x01u
#define TSR_TRANSMITTED  0x01u
#define RSR_RECEIVED     0x01u

#define PAGE_SIZE        256u
#define BUFFER_START     0x4000u
#define PROM_SIZE        32u
#define PROM_SIGNATURE   0x57u
#define MIN_FRAME        60u
#define CRC_LENGTH       4u
#define RECEIVE_HEADER   4u

#define PORT_MASK        0x1Fu
#define PORT_DATA        0x10u
#define PORT_RESET       0x18u

#define COR_OFFSET       0x200u
#define COR_SRESET       0x80u
#define COR_INDEX        0x3Fu

static const uint8_t cis_template[] = {
    0x01, 0x02, 0x00, 0xFF,
    0x15, 0x34, 0x04, 0x01,
    'L', 'o', 'w', ' ', 'P', 'o', 'w', 'e', 'r', ' ', 'E', 't', 'h', 'e', 'r', 'n', 'e', 't', ' ', 'C', 'F', 0x00,
    'S', 'o', 'c', 'k', 'e', 't', ' ', 'C', 'o', 'm', 'm', 'u', 'n', 'i', 'c', 'a', 't', 'i', 'o', 'n', 's', ',', ' ', 'I', 'n', 'c', 0x00,
    0xFF,
    0x20, 0x04, 0x04, 0x01, 0x2E, 0x44,
    0x21, 0x02, 0x06, 0x00,
    0x22, 0x08, 0x04, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x1A, 0x05, 0x01, 0x01, 0x00, 0x02, 0x01,
    0x1B, 0x0B, 0xC1, 0x01, 0x18, 0xE5, 0x60, 0x00, 0x03, 0x1F, 0x30, 0xFF, 0xFF,
    0xFF, 0x00,
};

static uint32_t cis_mac_offset(void) {
    uint32_t offset = 0;
    while (offset < sizeof cis_template && cis_template[offset] != 0x22) offset += 2u + cis_template[offset + 1];
    return offset + 4;
}

static uint8_t cis_byte(const ne2000_t *card, uint32_t index) {
    if (index >= sizeof cis_template) return 0;
    uint32_t mac = cis_mac_offset();
    if (index >= mac && index < mac + 6) return card->mac[index - mac];
    return cis_template[index];
}

static uint8_t prom_byte(const ne2000_t *card, uint32_t address) {
    uint32_t index = (address % PROM_SIZE) / 2;
    if (index < 6) return card->mac[index];
    return index >= 14 ? PROM_SIGNATURE : 0;
}

static uint8_t memory_read(const ne2000_t *card, uint32_t address) {
    address &= 0xFFFFu;
    if (address < PROM_SIZE) return prom_byte(card, address);
    if (address >= BUFFER_START && address < NE2000_MEMORY) return card->memory[address];
    return 0xFF;
}

static void memory_write(ne2000_t *card, uint32_t address, uint8_t value) {
    address &= 0xFFFFu;
    if (address >= BUFFER_START && address < NE2000_MEMORY) card->memory[address] = value;
}

void ne2000_reset(ne2000_t *card) {
    card->command = CR_STOP | CR_DMA_ABORT;
    card->interrupt_status = ISR_RESET;
    card->interrupt_mask = 0;
    card->data_config = 0;
    card->receive_config = 0;
    card->transmit_config = 0;
    card->remote_count = 0;
    card->transmit_status = 0;
    card->receive_status = 0;
}

void ne2000_power_on(ne2000_t *card) {
    if (!card->inserted) return;
    card->cor = 0;
    card->sent_count = 0;
    ne2000_reset(card);
}

void ne2000_insert(ne2000_t *card, const uint8_t mac[6]) {
    memset(card, 0, sizeof *card);
    card->inserted = true;
    memcpy(card->mac, mac, 6);
    ne2000_reset(card);
}

void ne2000_eject(ne2000_t *card) {
    card->inserted = false;
}

void ne2000_sanitize(ne2000_t *card) {
    if (card->sent_head >= NE2000_FRAMES || card->sent_count > NE2000_FRAMES) card->sent_head = card->sent_count = 0;
    for (int i = 0; i < NE2000_FRAMES; i++) {
        if (card->sent_length[i] > NE2000_FRAME_MAX) card->sent_length[i] = NE2000_FRAME_MAX;
    }
}

bool ne2000_io_mode(const ne2000_t *card) {
    return card->inserted && (card->cor & COR_INDEX) != 0;
}

bool ne2000_interrupt(const ne2000_t *card) {
    return ne2000_io_mode(card) && (card->interrupt_status & card->interrupt_mask & ISR_SOURCES) != 0;
}

uint32_t ne2000_attribute_read(ne2000_t *card, uint32_t offset, int size) {
    if (!card->inserted) return size == 1 ? 0xFFu : 0xFFFFu;
    if (offset & 1) return 0xFF;
    uint32_t even = offset & ~1u;
    uint8_t value = even == COR_OFFSET ? card->cor : cis_byte(card, even / 2);
    return size == 1 ? value : (uint32_t)(0xFF00u | value);
}

void ne2000_attribute_write(ne2000_t *card, uint32_t offset, int size, uint32_t value) {
    (void)size;
    if (!card->inserted || offset != COR_OFFSET) return;
    if (value & COR_SRESET) {
        ne2000_reset(card);
        card->cor = 0;
        return;
    }
    card->cor = (uint8_t)value;
}

static void start_remote(ne2000_t *card) {
    card->remote_address = card->remote_start;
}

static void remote_step(ne2000_t *card, uint32_t bytes) {
    uint32_t before = card->remote_address, after = before + bytes, ring_end = (uint32_t)card->page_stop * PAGE_SIZE;
    if (ring_end && before < ring_end && after >= ring_end) after = (uint32_t)card->page_start * PAGE_SIZE + (after - ring_end);
    card->remote_address = (uint16_t)after;
    card->remote_count = bytes >= card->remote_count ? 0 : (uint16_t)(card->remote_count - bytes);
    if (!card->remote_count) card->interrupt_status |= ISR_DMA_COMPLETE;
}

static uint32_t data_read(ne2000_t *card, int size) {
    uint32_t value = memory_read(card, card->remote_address);
    if (size == 1 && !(card->data_config & DCR_WORD)) {
        remote_step(card, 1);
        return value;
    }
    value |= (uint32_t)memory_read(card, card->remote_address + 1u) << 8;
    remote_step(card, 2);
    return value;
}

static void data_write(ne2000_t *card, int size, uint32_t value) {
    memory_write(card, card->remote_address, (uint8_t)value);
    if (size == 1 && !(card->data_config & DCR_WORD)) {
        remote_step(card, 1);
        return;
    }
    memory_write(card, card->remote_address + 1u, (uint8_t)(value >> 8));
    remote_step(card, 2);
}

static void transmit(ne2000_t *card) {
    uint32_t length = card->transmit_count, start = (uint32_t)card->transmit_page * PAGE_SIZE;
    if (length > NE2000_FRAME_MAX) length = NE2000_FRAME_MAX;
    if (card->sent_count < NE2000_FRAMES && length > 0) {
        uint32_t slot = (card->sent_head + card->sent_count) % NE2000_FRAMES;
        for (uint32_t i = 0; i < length; i++) card->sent[slot][i] = memory_read(card, start + i);
        card->sent_length[slot] = (uint16_t)length;
        card->sent_count++;
    }
    card->transmit_status = TSR_TRANSMITTED;
    card->interrupt_status |= ISR_TRANSMITTED;
    card->command &= (uint8_t) ~CR_TRANSMIT;
}

static void write_command(ne2000_t *card, uint8_t value) {
    card->command = value;
    if (value & CR_STOP) card->interrupt_status |= ISR_RESET;
    else if (value & CR_START) card->interrupt_status &= (uint8_t) ~ISR_RESET;
    uint8_t dma = value & CR_DMA_MASK;
    if (dma == CR_DMA_READ || dma == CR_DMA_WRITE) {
        start_remote(card);
        if (!card->remote_count) card->interrupt_status |= ISR_DMA_COMPLETE;
    }
    if ((value & CR_TRANSMIT) && (value & CR_START)) transmit(card);
}

static uint8_t register_read(ne2000_t *card, uint32_t reg) {
    uint32_t page = card->command >> CR_PAGE_SHIFT;
    if (reg == 0) return card->command;
    if (page == 1) {
        if (reg <= 6) return card->physical[reg - 1];
        if (reg == 7) return card->current_page;
        return card->multicast[reg - 8];
    }
    if (page == 2) {
        switch (reg) {
        case 0x1: return card->page_start;
        case 0x2: return card->page_stop;
        case 0x4: return card->transmit_page;
        case 0xC: return card->receive_config;
        case 0xD: return card->transmit_config;
        case 0xE: return card->data_config;
        case 0xF: return card->interrupt_mask;
        default: return 0xFF;
        }
    }
    switch (reg) {
    case 0x3: return card->boundary;
    case 0x4: return card->transmit_status;
    case 0x7: return card->interrupt_status;
    case 0x8: return (uint8_t)card->remote_address;
    case 0x9: return (uint8_t)(card->remote_address >> 8);
    case 0xC: return card->receive_status;
    default: return 0;
    }
}

static void register_write(ne2000_t *card, uint32_t reg, uint8_t value) {
    uint32_t page = card->command >> CR_PAGE_SHIFT;
    if (reg == 0) {
        write_command(card, value);
        return;
    }
    if (page == 1) {
        if (reg <= 6) card->physical[reg - 1] = value;
        else if (reg == 7) card->current_page = value;
        else card->multicast[reg - 8] = value;
        return;
    }
    if (page != 0) return;
    switch (reg) {
    case 0x1: card->page_start = value; break;
    case 0x2: card->page_stop = value; break;
    case 0x3: card->boundary = value; break;
    case 0x4: card->transmit_page = value; break;
    case 0x5: card->transmit_count = (uint16_t)((card->transmit_count & 0xFF00u) | value); break;
    case 0x6: card->transmit_count = (uint16_t)((card->transmit_count & 0x00FFu) | value << 8); break;
    case 0x7: card->interrupt_status &= (uint8_t) ~value; break;
    case 0x8: card->remote_start = (uint16_t)((card->remote_start & 0xFF00u) | value); break;
    case 0x9: card->remote_start = (uint16_t)((card->remote_start & 0x00FFu) | value << 8); break;
    case 0xA: card->remote_count = (uint16_t)((card->remote_count & 0xFF00u) | value); break;
    case 0xB: card->remote_count = (uint16_t)((card->remote_count & 0x00FFu) | value << 8); break;
    case 0xC: card->receive_config = value; break;
    case 0xD: card->transmit_config = value; break;
    case 0xE: card->data_config = value; break;
    case 0xF: card->interrupt_mask = value; break;
    default: break;
    }
}

uint32_t ne2000_io_read(ne2000_t *card, uint32_t offset, int size) {
    if (!ne2000_io_mode(card)) return size == 1 ? 0xFFu : 0xFFFFu;
    uint32_t port = offset & PORT_MASK;
    if (port >= PORT_RESET) {
        ne2000_reset(card);
        return 0;
    }
    if (port >= PORT_DATA) {
        uint32_t value = data_read(card, size == 1 ? 1 : 2);
        if (size == 4) value |= data_read(card, 2) << 16;
        return value;
    }
    uint32_t value = 0;
    for (int i = 0; i < size; i++) value |= (uint32_t)register_read(card, port + (uint32_t)i) << (8 * i);
    return value;
}

void ne2000_io_write(ne2000_t *card, uint32_t offset, int size, uint32_t value) {
    if (!ne2000_io_mode(card)) return;
    uint32_t port = offset & PORT_MASK;
    if (port >= PORT_RESET) return;
    if (port >= PORT_DATA) {
        data_write(card, size == 1 ? 1 : 2, value);
        if (size == 4) data_write(card, 2, value >> 16);
        return;
    }
    for (int i = 0; i < size; i++) register_write(card, port + (uint32_t)i, (uint8_t)(value >> (8 * i)));
}

static bool accepts(const ne2000_t *card, const uint8_t *frame) {
    static const uint8_t broadcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (card->receive_config & RCR_PROMISCUOUS) return true;
    if (!memcmp(frame, broadcast, 6)) return (card->receive_config & RCR_BROADCAST) != 0;
    if (frame[0] & 1) return (card->receive_config & RCR_MULTICAST) != 0;
    return !memcmp(frame, card->physical, 6);
}

bool ne2000_receive(ne2000_t *card, const uint8_t *frame, size_t length) {
    if (!ne2000_io_mode(card) || (card->command & CR_STOP) || !(card->command & CR_START)) return false;
    if (length < 6 || length > NE2000_FRAME_MAX || !accepts(card, frame)) return false;
    uint32_t start = card->page_start, stop = card->page_stop;
    if (stop <= start || start * PAGE_SIZE < BUFFER_START || stop * PAGE_SIZE > NE2000_MEMORY) return false;
    uint32_t stored = (uint32_t)(length < MIN_FRAME ? MIN_FRAME : length) + CRC_LENGTH;
    uint32_t pages = (stored + RECEIVE_HEADER + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t current = card->current_page, boundary = card->boundary;
    uint32_t free_pages = boundary > current ? boundary - current : (stop - start) - (current - boundary);
    if (pages >= free_pages) {
        card->interrupt_status |= ISR_OVERWRITE;
        return false;
    }
    uint32_t next = current + pages;
    if (next >= stop) next -= stop - start;
    uint32_t address = current * PAGE_SIZE;
    uint8_t header[RECEIVE_HEADER] = { RSR_RECEIVED, (uint8_t)next, (uint8_t)stored, (uint8_t)(stored >> 8) };
    for (uint32_t i = 0; i < RECEIVE_HEADER + stored - CRC_LENGTH; i++) {
        uint8_t byte = i < RECEIVE_HEADER ? header[i] : i - RECEIVE_HEADER < length ? frame[i - RECEIVE_HEADER] : 0;
        card->memory[address] = byte;
        if (++address >= stop * PAGE_SIZE) address = start * PAGE_SIZE;
    }
    card->current_page = (uint8_t)next;
    card->receive_status = RSR_RECEIVED;
    card->interrupt_status |= ISR_RECEIVED;
    return true;
}

size_t ne2000_take_sent(ne2000_t *card, uint8_t *frame, size_t max) {
    if (!card->sent_count) return 0;
    size_t length = card->sent_length[card->sent_head];
    if (length > max) length = max;
    memcpy(frame, card->sent[card->sent_head], length);
    card->sent_head = (card->sent_head + 1) % NE2000_FRAMES;
    card->sent_count--;
    return length;
}

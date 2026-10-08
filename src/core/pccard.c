#include "core/pccard.h"

#include <string.h>

#define REG_GPIO_DATAOUT    0x00
#define REG_MFIO_DATAOUT    0x02
#define REG_GPIO_DIR        0x04
#define REG_MFIO_DIR        0x06
#define REG_MFIO_SEL        0x0A
#define REG_GPIO_DATAIN     0x0C
#define REG_MFIO_DATAIN     0x0E
#define REG_GPIO_POSINTEN   0x10
#define REG_MFIO_POSINTEN   0x12
#define REG_GPIO_NEGINTEN   0x14
#define REG_MFIO_NEGINTEN   0x16
#define REG_GPIO_POSINTSTAT 0x18
#define REG_MFIO_POSINTSTAT 0x1A
#define REG_GPIO_NEGINTSTAT 0x1C
#define REG_MFIO_NEGINTSTAT 0x1E
#define REG_CTRL            0x20
#define REG_MMODULE_ID      0x22

#define MMODULE_ID 0x4900u

#define GPIO_MASK     0x1FFFu
#define PIN_CRDDET2   0x0800u
#define PIN_CRDDET1   0x0400u
#define PIN_CRDVCCON  0x00C0u
#define PIN_BCRDWP    0x0008u
#define PIN_BCRDRDY   0x0004u
#define PIN_BCRDRST   0x0001u
#define PIN_CRDDET    (PIN_CRDDET1 | PIN_CRDDET2)

#define CTRL_CARDEN    0x0004u
#define CTRL_GLOBALEN  0x0002u
#define CTRL_SOFTRESET 0x0040u

#define CARD_SHIFT       26
#define FIXATTR_SHIFT    25
#define FIXATTR_MASK     0x01FFFFFFu
#define COMMON_MASK      0x03FFFFFFu

#define ATA_BUSY  0x80u
#define ATA_READY 0x40u
#define ATA_SEEK  0x10u
#define ATA_DRQ   0x08u
#define ATA_ERROR 0x01u

#define ATA_READ     0x20u
#define ATA_WRITE    0x30u
#define ATA_IDENTIFY 0xECu
#define ATA_RECALIBRATE 0x10u
#define ATA_SEEK_CMD 0x70u
#define ATA_SET_PARAMETERS 0x91u
#define ATA_IDLE     0x97u

#define HEAD_LBA 0x40u
#define ATA_MAX_SECTORS 256u

#define REG_DATA     0
#define REG_FEATURE  1
#define REG_SECTORS  2
#define REG_SECTOR   3
#define REG_CYL_LOW  4
#define REG_CYL_HIGH 5
#define REG_DRV_HEAD 6
#define REG_COMMAND  7
#define REG_DEVCTL   0x0E

#define COR_OFFSET 0x200u
#define CHS_HEADS   16u
#define CHS_SECTORS 63u

static const uint8_t cis[] = {
    0x01, 0x03, 0xDB, 0x00, 0xFF,
    0x17, 0x03, 0xDB, 0x00, 0xFF,
    0x20, 0x04, 0x45, 0x00, 0x01, 0x04,
    0x21, 0x02, 0x04, 0x00,
    0x22, 0x02, 0x01, 0x01,
    0x1A, 0x05, 0x01, 0x03, 0x00, 0x02, 0x01,
    0x1B, 0x05, 0x80, 0x40, 0x01, 0x01, 0x55,
    0x1B, 0x0B, 0xC1, 0x41, 0x09, 0x01, 0x55, 0xE4, 0x51, 0x00, 0x07, 0x0E, 0x01,
    0x1B, 0x0D, 0x82, 0x41, 0x09, 0x01, 0x55, 0xEA, 0x61, 0xF0, 0x01, 0x07, 0xF6, 0x03, 0x01,
    0x1B, 0x0D, 0x83, 0x41, 0x09, 0x01, 0x55, 0xEA, 0x61, 0x70, 0x01, 0x07, 0x76, 0x03, 0x01,
    0x14, 0x00,
    0xFF, 0x00,
};

static uint16_t gpio_datain(const pccard_t *card) {
    uint16_t driven = card->gpio_dataout & card->gpio_dir;
    uint16_t in = driven | (GPIO_MASK & ~card->gpio_dir);
    if (!card->inserted) return in;
    in &= (uint16_t) ~PIN_CRDDET;
    if (!card->powered) return in;
    in &= (uint16_t) ~PIN_BCRDWP;
    if (card->card_irq) in &= (uint16_t) ~PIN_BCRDRDY;
    else in |= PIN_BCRDRDY;
    return in;
}

static void update_int(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    bool level = (card->ctrl & CTRL_GLOBALEN) &&
                 ((card->gpio_posintstat & card->gpio_posinten) | (card->gpio_negintstat & card->gpio_neginten));
    if (level == card->int_asserted) return;
    card->int_asserted = level;
    if (socket->int_changed) socket->int_changed(socket->context, level);
}

static void latch_edges(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    uint16_t now = gpio_datain(card);
    uint16_t diff = now ^ card->prev_datain;
    card->gpio_posintstat |= diff & now;
    card->gpio_negintstat |= diff & (uint16_t) ~now;
    card->prev_datain = now;
    update_int(socket);
}

static void set_card_irq(pccard_socket_t *socket, bool asserted) {
    if (socket->state->card_irq == asserted) return;
    socket->state->card_irq = asserted;
    latch_edges(socket);
}

static void irq_assert(pccard_socket_t *socket) {
    if (socket->state->device_control & 0x02u) return;
    set_card_irq(socket, true);
}

static void irq_clear(pccard_socket_t *socket) {
    set_card_irq(socket, false);
}

static void card_power(pccard_socket_t *socket, bool on) {
    pccard_t *card = socket->state;
    if (card->powered == on) return;
    card->powered = on;
    if (!card->inserted) return;
    card->sectors_left = 0;
    card->buffer_position = 0;
    card->writing = false;
    card->card_irq = false;
    if (on) {
        card->drive_head = 0xA0;
        card->error = 0x01;
        card->status = socket->image ? (ATA_READY | ATA_SEEK) : 0;
    } else {
        card->status = 0;
    }
}

static void card_socket_reset(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    if (!card->inserted || !card->powered) return;
    card->cor = 0;
    card->sectors_left = 0;
    card->buffer_position = 0;
    card->writing = false;
    card->drive_head = 0xA0;
    card->error = 0x01;
    card->status = socket->image ? (ATA_READY | ATA_SEEK) : 0;
    irq_clear(socket);
}

static void apply_outputs(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    uint16_t driven = card->gpio_dataout & card->gpio_dir;
    card_power(socket, (driven & PIN_CRDVCCON) != 0);
    bool reset_now = (driven & PIN_BCRDRST) != 0;
    bool release = card->reset_asserted && !reset_now;
    card->reset_asserted = reset_now;
    if (release) card_socket_reset(socket);
    latch_edges(socket);
}

void pccard_reset(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    bool inserted = card->inserted;
    uint64_t total = card->total_sectors;
    memset(card, 0, sizeof *card);
    card->inserted = inserted;
    card->total_sectors = total;
    card->prev_datain = GPIO_MASK;
    card->sector_count = 1;
    card->sector_number = 1;
    card->drive_head = 0xA0;
}

bool pccard_insert(pccard_socket_t *socket, FILE *image) {
    if (socket->state->inserted) pccard_eject(socket);
    if (!image) return false;
    fseek(image, 0, SEEK_END);
    long bytes = ftell(image);
    socket->image = image;
    socket->state->total_sectors = bytes > 0 ? (uint64_t)bytes / 512 : 0;
    socket->state->inserted = true;
    socket->state->powered = false;
    socket->state->card_irq = false;
    socket->state->cor = 0;
    apply_outputs(socket);
    return true;
}

void pccard_eject(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    if (!card->inserted) return;
    card_power(socket, false);
    card->inserted = false;
    card->card_irq = false;
    if (socket->image) fclose(socket->image);
    socket->image = NULL;
    apply_outputs(socket);
}

void pccard_rebind(pccard_socket_t *socket, FILE *image) {
    if (socket->image && socket->image != image) fclose(socket->image);
    socket->image = image;
    if (socket->state->inserted && !image) {
        socket->state->inserted = false;
        socket->state->card_irq = false;
        socket->state->powered = false;
        latch_edges(socket);
    }
}

void pccard_sanitize(pccard_t *card) {
    bool transferring = (card->status & ATA_DRQ) != 0;
    bool position_valid = transferring ? card->buffer_position < sizeof card->buffer : card->buffer_position <= sizeof card->buffer;
    bool count_valid = card->sectors_left <= ATA_MAX_SECTORS && (!transferring || card->sectors_left > 0);
    if (position_valid && count_valid) return;
    card->status &= (uint8_t) ~ATA_DRQ;
    card->buffer_position = 0;
    card->sectors_left = 0;
    card->writing = false;
}

uint16_t pccard_it8368_read(pccard_socket_t *socket, uint32_t offset) {
    pccard_t *card = socket->state;
    switch (offset) {
    case REG_GPIO_DATAOUT: return card->gpio_dataout;
    case REG_GPIO_DIR: return card->gpio_dir;
    case REG_MFIO_DATAOUT: return card->mfio_dataout;
    case REG_MFIO_DIR: return card->mfio_dir;
    case REG_MFIO_SEL: return card->mfio_sel;
    case REG_GPIO_DATAIN: latch_edges(socket); return gpio_datain(card);
    case REG_MFIO_DATAIN: return card->mfio_dataout & card->mfio_dir;
    case REG_GPIO_POSINTEN: return card->gpio_posinten;
    case REG_GPIO_NEGINTEN: return card->gpio_neginten;
    case REG_GPIO_POSINTSTAT: latch_edges(socket); return card->gpio_posintstat;
    case REG_GPIO_NEGINTSTAT: latch_edges(socket); return card->gpio_negintstat;
    case REG_MFIO_POSINTSTAT: return card->mfio_posintstat;
    case REG_MFIO_NEGINTSTAT: return card->mfio_negintstat;
    case REG_CTRL: return card->ctrl;
    case REG_MMODULE_ID: return MMODULE_ID;
    default: return 0;
    }
}

void pccard_it8368_write(pccard_socket_t *socket, uint32_t offset, uint16_t value) {
    pccard_t *card = socket->state;
    switch (offset) {
    case REG_GPIO_DATAOUT: card->gpio_dataout = value; apply_outputs(socket); return;
    case REG_GPIO_DIR: card->gpio_dir = value; apply_outputs(socket); return;
    case REG_MFIO_DATAOUT: card->mfio_dataout = value; return;
    case REG_MFIO_DIR: card->mfio_dir = value; return;
    case REG_MFIO_SEL: card->mfio_sel = value; return;
    case REG_GPIO_POSINTEN: card->gpio_posinten = value & GPIO_MASK; update_int(socket); return;
    case REG_GPIO_NEGINTEN: card->gpio_neginten = value & GPIO_MASK; update_int(socket); return;
    case REG_GPIO_POSINTSTAT: card->gpio_posintstat &= (uint16_t) ~value; update_int(socket); return;
    case REG_GPIO_NEGINTSTAT: card->gpio_negintstat &= (uint16_t) ~value; update_int(socket); return;
    case REG_MFIO_POSINTSTAT: card->mfio_posintstat &= (uint16_t) ~value; return;
    case REG_MFIO_NEGINTSTAT: card->mfio_negintstat &= (uint16_t) ~value; return;
    case REG_CTRL:
        if (value & CTRL_SOFTRESET) {
            card->gpio_dataout = card->gpio_dir = 0;
            card->gpio_posinten = card->gpio_neginten = 0;
            card->gpio_posintstat = card->gpio_negintstat = 0;
            card->mfio_posintstat = card->mfio_negintstat = 0;
            card->mfio_dataout = card->mfio_dir = card->mfio_sel = 0;
            card->ctrl = 0;
            apply_outputs(socket);
            return;
        }
        card->ctrl = value;
        update_int(socket);
        return;
    default:
        return;
    }
}

static uint64_t current_lba(const pccard_t *card) {
    if (card->drive_head & HEAD_LBA) {
        return ((uint64_t)(card->drive_head & 0x0F) << 24) | ((uint64_t)card->cylinder_high << 16) |
               ((uint64_t)card->cylinder_low << 8) | card->sector_number;
    }
    uint32_t cylinder = (uint32_t)card->cylinder_high << 8 | card->cylinder_low;
    uint32_t head = card->drive_head & 0x0F;
    return ((uint64_t)cylinder * CHS_HEADS + head) * CHS_SECTORS + (card->sector_number ? card->sector_number - 1u : 0u);
}

static void advance_sector(pccard_t *card) {
    uint64_t lba = current_lba(card) + 1;
    if (card->drive_head & HEAD_LBA) {
        card->sector_number = (uint8_t)lba;
        card->cylinder_low = (uint8_t)(lba >> 8);
        card->cylinder_high = (uint8_t)(lba >> 16);
        card->drive_head = (uint8_t)((card->drive_head & 0xF0) | ((lba >> 24) & 0x0F));
    } else {
        uint32_t sector = (uint32_t)(lba % CHS_SECTORS) + 1;
        uint32_t rest = (uint32_t)(lba / CHS_SECTORS);
        card->sector_number = (uint8_t)sector;
        card->cylinder_low = (uint8_t)(rest / CHS_HEADS);
        card->cylinder_high = (uint8_t)((rest / CHS_HEADS) >> 8);
        card->drive_head = (uint8_t)((card->drive_head & 0xF0) | (rest % CHS_HEADS));
    }
}

static bool read_sector(pccard_socket_t *socket, uint64_t lba, uint8_t *out) {
    if (!socket->image || lba >= socket->state->total_sectors) return false;
    if (fseek(socket->image, (long)(lba * 512), SEEK_SET) != 0) return false;
    return fread(out, 1, 512, socket->image) == 512;
}

static bool write_sector(pccard_socket_t *socket, uint64_t lba, const uint8_t *in) {
    if (!socket->image || lba >= socket->state->total_sectors) return false;
    if (fseek(socket->image, (long)(lba * 512), SEEK_SET) != 0) return false;
    bool ok = fwrite(in, 1, 512, socket->image) == 512;
    fflush(socket->image);
    return ok;
}

static void put_ata_string(uint8_t *destination, const char *text, size_t bytes) {
    size_t length = strlen(text);
    for (size_t i = 0; i < bytes; i++) destination[i ^ 1] = i < length ? (uint8_t)text[i] : ' ';
}

static void build_identify(pccard_t *card) {
    memset(card->buffer, 0, sizeof card->buffer);
    uint16_t words[256] = { 0 };
    uint32_t cylinders = (uint32_t)(card->total_sectors / (CHS_HEADS * CHS_SECTORS));
    if (cylinders > 0xFFFF) cylinders = 0xFFFF;
    words[0] = 0x848A;
    words[1] = (uint16_t)cylinders;
    words[3] = CHS_HEADS;
    words[6] = CHS_SECTORS;
    words[47] = 0x8001;
    words[49] = 0x0200;
    words[51] = 0x0200;
    words[53] = 0x0001;
    words[54] = words[1];
    words[55] = words[3];
    words[56] = words[6];
    uint32_t capacity = CHS_HEADS * CHS_SECTORS * cylinders;
    words[57] = (uint16_t)capacity;
    words[58] = (uint16_t)(capacity >> 16);
    uint32_t lba28 = card->total_sectors > 0x0FFFFFFF ? 0x0FFFFFFF : (uint32_t)card->total_sectors;
    words[60] = (uint16_t)lba28;
    words[61] = (uint16_t)(lba28 >> 16);
    for (int i = 0; i < 256; i++) {
        card->buffer[i * 2] = (uint8_t)words[i];
        card->buffer[i * 2 + 1] = (uint8_t)(words[i] >> 8);
    }
    put_ata_string(card->buffer + 20, "VELO00000001", 20);
    put_ata_string(card->buffer + 46, "1.0", 8);
    put_ata_string(card->buffer + 54, "velo-emu CompactFlash", 40);
}

static void finish_data_block(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    if (--card->sectors_left == 0) {
        card->status = ATA_READY | ATA_SEEK;
        return;
    }
    advance_sector(card);
    if (read_sector(socket, current_lba(card), card->buffer)) {
        card->buffer_position = 0;
    } else {
        card->error = 0x10;
        card->status = ATA_READY | ATA_ERROR;
        card->sectors_left = 0;
    }
    irq_assert(socket);
}

static void complete_write_sector(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    if (!write_sector(socket, current_lba(card), card->buffer)) {
        card->error = 0x10;
        card->status = ATA_READY | ATA_ERROR;
        card->sectors_left = 0;
        card->writing = false;
        irq_assert(socket);
        return;
    }
    if (--card->sectors_left == 0) {
        card->status = ATA_READY | ATA_SEEK;
        card->writing = false;
    } else {
        advance_sector(card);
        card->buffer_position = 0;
        card->status = ATA_READY | ATA_SEEK | ATA_DRQ;
    }
    irq_assert(socket);
}

static uint8_t read_data8(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    if (!(card->status & ATA_DRQ) || card->writing) return 0xFF;
    uint8_t value = card->buffer[card->buffer_position++];
    if (card->buffer_position >= 512) finish_data_block(socket);
    return value;
}

static uint16_t read_data16(pccard_socket_t *socket) {
    pccard_t *card = socket->state;
    if (!(card->status & ATA_DRQ) || card->writing) return 0xFFFF;
    if (card->buffer_position + 1 >= sizeof card->buffer) return (uint16_t)(0xFF00u | read_data8(socket));
    uint16_t value = (uint16_t)(card->buffer[card->buffer_position] | card->buffer[card->buffer_position + 1] << 8);
    card->buffer_position += 2;
    if (card->buffer_position >= 512) finish_data_block(socket);
    return value;
}

static void write_data8(pccard_socket_t *socket, uint8_t value) {
    pccard_t *card = socket->state;
    if (!card->writing || !(card->status & ATA_DRQ)) return;
    card->buffer[card->buffer_position++] = value;
    if (card->buffer_position >= 512) complete_write_sector(socket);
}

static void write_data16(pccard_socket_t *socket, uint16_t value) {
    pccard_t *card = socket->state;
    if (!card->writing || !(card->status & ATA_DRQ)) return;
    if (card->buffer_position + 1 >= sizeof card->buffer) {
        write_data8(socket, (uint8_t)value);
        return;
    }
    card->buffer[card->buffer_position] = (uint8_t)value;
    card->buffer[card->buffer_position + 1] = (uint8_t)(value >> 8);
    card->buffer_position += 2;
    if (card->buffer_position >= 512) complete_write_sector(socket);
}

static uint8_t read_register(pccard_socket_t *socket, int reg) {
    pccard_t *card = socket->state;
    switch (reg) {
    case REG_DATA: return read_data8(socket);
    case REG_FEATURE: return card->error;
    case REG_SECTORS: return card->sector_count;
    case REG_SECTOR: return card->sector_number;
    case REG_CYL_LOW: return card->cylinder_low;
    case REG_CYL_HIGH: return card->cylinder_high;
    case REG_DRV_HEAD: return card->drive_head;
    case REG_COMMAND: irq_clear(socket); return card->status;
    case REG_DEVCTL: return card->status;
    default: return 0xFF;
    }
}

static void command(pccard_socket_t *socket, uint8_t value) {
    pccard_t *card = socket->state;
    irq_clear(socket);
    card->error = 0;
    if (value == ATA_IDENTIFY) {
        build_identify(card);
        card->buffer_position = 0;
        card->sectors_left = 1;
        card->writing = false;
        card->status = ATA_READY | ATA_SEEK | ATA_DRQ;
        irq_assert(socket);
    } else if (value == ATA_READ) {
        card->sectors_left = card->sector_count ? card->sector_count : ATA_MAX_SECTORS;
        card->writing = false;
        if (read_sector(socket, current_lba(card), card->buffer)) {
            card->buffer_position = 0;
            card->status = ATA_READY | ATA_SEEK | ATA_DRQ;
        } else {
            card->error = 0x10;
            card->status = ATA_READY | ATA_ERROR;
            card->sectors_left = 0;
        }
        irq_assert(socket);
    } else if (value == ATA_WRITE) {
        card->sectors_left = card->sector_count ? card->sector_count : ATA_MAX_SECTORS;
        card->writing = true;
        card->buffer_position = 0;
        card->status = ATA_READY | ATA_SEEK | ATA_DRQ;
    } else if ((value & 0xF0) == ATA_RECALIBRATE || value == ATA_SEEK_CMD || value == ATA_SET_PARAMETERS || value == ATA_IDLE) {
        card->status = ATA_READY | ATA_SEEK;
        irq_assert(socket);
    } else {
        card->error = 0x04;
        card->status = ATA_READY | ATA_ERROR;
        irq_assert(socket);
    }
}

static void write_register(pccard_socket_t *socket, int reg, uint8_t value) {
    pccard_t *card = socket->state;
    switch (reg) {
    case REG_DATA: write_data8(socket, value); return;
    case REG_FEATURE: card->feature = value; return;
    case REG_SECTORS: card->sector_count = value; return;
    case REG_SECTOR: card->sector_number = value; return;
    case REG_CYL_LOW: card->cylinder_low = value; return;
    case REG_CYL_HIGH: card->cylinder_high = value; return;
    case REG_DRV_HEAD: card->drive_head = value; return;
    case REG_COMMAND: command(socket, value); return;
    case REG_DEVCTL: card->device_control = value; return;
    default: return;
    }
}

static int decode_io(const pccard_t *card, uint32_t offset) {
    uint8_t index = card->cor & 0x3F;
    if (index == 2 || index == 3) {
        uint32_t address = offset & 0x3FF;
        uint32_t base = index == 2 ? 0x1F0 : 0x170;
        uint32_t control = index == 2 ? 0x3F6 : 0x376;
        if (address >= base && address <= base + 7) return (int)(address - base);
        if (address == control) return REG_DEVCTL;
        return -1;
    }
    uint32_t address = offset & 0x0F;
    return (address <= 7 || address == REG_DEVCTL) ? (int)address : -1;
}

static int decode_common(const pccard_t *card, uint32_t offset) {
    if ((card->cor & 0x3F) != 0 || (offset > 7 && offset != REG_DEVCTL)) return -1;
    return (int)offset;
}

static uint8_t attribute_read(const pccard_t *card, uint32_t offset) {
    if (offset == COR_OFFSET) return card->cor;
    if (offset < sizeof cis * 2) return cis[offset / 2];
    return 0;
}

static bool card_live(const pccard_socket_t *socket) {
    return socket->state->inserted && socket->state->powered;
}

static uint16_t ctrl_read16(pccard_socket_t *socket, uint32_t offset) {
    pccard_t *card = socket->state;
    if (!(card->ctrl & CTRL_CARDEN) || ((offset >> CARD_SHIFT) & 1) || !card_live(socket)) return 0xFFFF;
    bool io = ((offset >> FIXATTR_SHIFT) & 1) == 0;
    uint32_t card_offset = offset & FIXATTR_MASK;
    if (!io) {
        uint8_t value = attribute_read(card, card_offset & ~1u);
        return (uint16_t)(value << 8 | value);
    }
    int reg = decode_io(card, card_offset);
    if (reg == REG_DATA) return read_data16(socket);
    return reg < 0 ? 0xFFFF : read_register(socket, reg);
}

static uint8_t ctrl_read8(pccard_socket_t *socket, uint32_t offset) {
    pccard_t *card = socket->state;
    if (!(card->ctrl & CTRL_CARDEN) || ((offset >> CARD_SHIFT) & 1) || !card_live(socket)) return 0xFF;
    bool io = ((offset >> FIXATTR_SHIFT) & 1) == 0;
    uint32_t card_offset = offset & FIXATTR_MASK;
    if (!io) return attribute_read(card, card_offset);
    int reg = decode_io(card, card_offset);
    return reg < 0 ? 0xFF : read_register(socket, reg);
}

static void ctrl_write(pccard_socket_t *socket, uint32_t offset, int size, uint16_t value) {
    pccard_t *card = socket->state;
    if (!(card->ctrl & CTRL_CARDEN) || ((offset >> CARD_SHIFT) & 1) || !card_live(socket)) return;
    bool io = ((offset >> FIXATTR_SHIFT) & 1) == 0;
    uint32_t card_offset = offset & FIXATTR_MASK;
    if (!io) {
        if ((size == 2 ? card_offset & ~1u : card_offset) == COR_OFFSET) card->cor = (uint8_t)value;
        return;
    }
    int reg = decode_io(card, card_offset);
    if (reg < 0) return;
    if (reg == REG_DATA && size == 2) write_data16(socket, value);
    else write_register(socket, reg, (uint8_t)value);
}

static uint16_t common_read(pccard_socket_t *socket, uint32_t offset, int size) {
    if (((offset >> CARD_SHIFT) & 1) || !card_live(socket)) return size == 1 ? 0xFF : 0xFFFF;
    int reg = decode_common(socket->state, offset & COMMON_MASK);
    if (reg < 0) return size == 1 ? 0xFF : 0xFFFF;
    if (reg == REG_DATA && size == 2) return read_data16(socket);
    return read_register(socket, reg);
}

static void common_write(pccard_socket_t *socket, uint32_t offset, int size, uint16_t value) {
    if (((offset >> CARD_SHIFT) & 1) || !card_live(socket)) return;
    int reg = decode_common(socket->state, offset & COMMON_MASK);
    if (reg < 0) return;
    if (reg == REG_DATA && size == 2) write_data16(socket, value);
    else write_register(socket, reg, (uint8_t)value);
}

uint32_t pccard_read(pccard_socket_t *socket, uint32_t pa, int size) {
    bool ctrl = pa < PCCARD_CTRL_WINDOW_END;
    uint32_t offset = pa - (ctrl ? PCCARD_CTRL_WINDOW_PA : PCCARD_MEM_WINDOW_PA);
    if (size == 4) {
        uint32_t low = ctrl ? ctrl_read16(socket, offset) : common_read(socket, offset, 2);
        uint32_t high = ctrl ? ctrl_read16(socket, offset + 2) : common_read(socket, offset + 2, 2);
        return low | high << 16;
    }
    if (size == 2) return ctrl ? ctrl_read16(socket, offset) : common_read(socket, offset, 2);
    return ctrl ? ctrl_read8(socket, offset) : common_read(socket, offset, 1);
}

void pccard_write(pccard_socket_t *socket, uint32_t pa, int size, uint32_t value) {
    bool ctrl = pa < PCCARD_CTRL_WINDOW_END;
    uint32_t offset = pa - (ctrl ? PCCARD_CTRL_WINDOW_PA : PCCARD_MEM_WINDOW_PA);
    if (size == 4) {
        if (ctrl) {
            ctrl_write(socket, offset, 2, (uint16_t)value);
            ctrl_write(socket, offset + 2, 2, (uint16_t)(value >> 16));
        } else {
            common_write(socket, offset, 2, (uint16_t)value);
            common_write(socket, offset + 2, 2, (uint16_t)(value >> 16));
        }
        return;
    }
    if (ctrl) ctrl_write(socket, offset, size, (uint16_t)value);
    else common_write(socket, offset, size, (uint16_t)value);
}

#include "core/uart.h"

#include "core/machine.h"

#define OFFSET_CTL1      0x00
#define OFFSET_CTL2      0x04
#define OFFSET_DMA_CTL1  0x08
#define OFFSET_DMA_CTL2  0x0C
#define OFFSET_DMA_COUNT 0x10
#define OFFSET_DATA      0x14

#define CTL1_UARTON   (1u << 31)
#define CTL1_EMPTY    (1u << 30)
#define CTL1_ENDMARX  (1u << 15)
#define CTL1_DISTXD   (1u << 6)
#define CTL1_TWOSTOP  (1u << 5)
#define CTL1_BIT7     (1u << 3)
#define CTL1_ENPARITY (1u << 1)
#define CTL1_ENUART   (1u << 0)
#define CTL1_WRITABLE 0x0000FFEFu

#define CTL2_BAUDRATE 0x000007FFu

#define UART_CLOCK_OVER_16 230400u

#define STATUS2_RX       (1u << 31)
#define STATUS2_TXAVAIL  (1u << 26)
#define STATUS2_TXEMPTY  (1u << 24)
#define STATUS2_DMAFULL  (1u << 23)
#define STATUS2_DMAHALF  (1u << 22)

#define NO_EVENT UINT64_MAX

uint32_t uart_space(const uart_port_t *port) {
    return UART_WIRE_SIZE - port->state->wire_count;
}

uint32_t uart_baud(const uart_port_t *port) {
    return UART_CLOCK_OVER_16 / (port->state->baud_divisor + 1);
}

static uint64_t cycles_per_byte(const uart_port_t *port) {
    uint32_t ctl1 = port->state->ctl1;
    uint32_t bits = 1 + ((ctl1 & CTL1_BIT7) ? 7 : 8) + ((ctl1 & CTL1_ENPARITY) ? 1 : 0) + ((ctl1 & CTL1_TWOSTOP) ? 2 : 1);
    return (uint64_t)MACHINE_CLOCK_HZ * bits / uart_baud(port);
}

static bool receiving(const uart_port_t *port) {
    const uart_t *uart = port->state;
    return (uart->ctl1 & CTL1_ENUART) && uart->dma_armed && uart->dma_length > 0;
}

uint32_t uart_read(uart_port_t *port, uint32_t offset) {
    uart_t *uart = port->state;
    switch (offset) {
    case OFFSET_CTL1: return (uart->ctl1 & CTL1_WRITABLE) | CTL1_EMPTY | ((uart->ctl1 & CTL1_ENUART) ? CTL1_UARTON : 0);
    case OFFSET_DMA_COUNT: return uart->dma_count;
    default: return 0;
    }
}

void uart_write(uart_port_t *port, uint32_t offset, uint32_t value, uint64_t now) {
    uart_t *uart = port->state;
    switch (offset) {
    case OFFSET_CTL1: {
        bool was_armed = uart->dma_armed;
        uart->ctl1 = value & CTL1_WRITABLE;
        uart->dma_armed = (value & CTL1_ENDMARX) != 0;
        if (uart->dma_armed && !was_armed) uart->dma_count = 0;
        if (receiving(port) && uart->wire_count && uart->rx_next == NO_EVENT) uart->rx_next = now + cycles_per_byte(port);
        if (!receiving(port)) uart->rx_next = NO_EVENT;
        return;
    }
    case OFFSET_CTL2: uart->baud_divisor = value & CTL2_BAUDRATE; return;
    case OFFSET_DMA_CTL1: uart->dma_buffer = value & ~3u; return;
    case OFFSET_DMA_CTL2: uart->dma_length = (value & 0xFFFF) + 1; return;
    case OFFSET_DATA:
        if (!(uart->ctl1 & CTL1_ENUART) || (uart->ctl1 & CTL1_DISTXD)) return;
        if (uart->tx_count < UART_TX_SIZE) {
            uart->tx[(uart->tx_head + uart->tx_count) % UART_TX_SIZE] = (uint8_t)value;
            uart->tx_count++;
        }
        port->raise(port->context, STATUS2_TXEMPTY | STATUS2_TXAVAIL);
        return;
    default:
        return;
    }
}

void uart_receive(uart_port_t *port, const uint8_t *data, uint32_t length, uint64_t now) {
    uart_t *uart = port->state;
    if (!receiving(port)) return;
    for (uint32_t i = 0; i < length && uart->wire_count < UART_WIRE_SIZE; i++) {
        uart->wire[(uart->wire_head + uart->wire_count) % UART_WIRE_SIZE] = data[i];
        uart->wire_count++;
    }
    if (uart->wire_count && uart->rx_next == NO_EVENT) uart->rx_next = now + cycles_per_byte(port);
}

uint64_t uart_next_event(const uart_port_t *port) {
    return port->state->rx_next;
}

void uart_event(uart_port_t *port, uint64_t now) {
    uart_t *uart = port->state;
    if (uart->rx_next > now) return;
    if (!receiving(port)) {
        uart->rx_next = NO_EVENT;
        return;
    }
    uint32_t bits = 0;
    uint64_t per_byte = cycles_per_byte(port);
    while (uart->wire_count && uart->rx_next <= now) {
        port->dram[(uart->dma_buffer + uart->dma_count) & port->dram_mask] = uart->wire[uart->wire_head];
        uart->wire_head = (uart->wire_head + 1) % UART_WIRE_SIZE;
        uart->wire_count--;
        uart->dma_count++;
        bits |= STATUS2_RX;
        if (uart->dma_count == uart->dma_length / 2) bits |= STATUS2_DMAHALF;
        if (uart->dma_count >= uart->dma_length) {
            bits |= STATUS2_DMAFULL;
            uart->dma_count = 0;
        }
        uart->rx_next += per_byte;
    }
    if (!uart->wire_count) uart->rx_next = NO_EVENT;
    if (bits) port->raise(port->context, bits);
}

void uart_sanitize(uart_t *uart) {
    uart->baud_divisor &= CTL2_BAUDRATE;
}

uint32_t uart_take_tx(uart_port_t *port, uint8_t *out, uint32_t max) {
    uart_t *uart = port->state;
    uint32_t count = uart->tx_count < max ? uart->tx_count : max;
    for (uint32_t i = 0; i < count; i++) out[i] = uart->tx[(uart->tx_head + i) % UART_TX_SIZE];
    uart->tx_head = (uart->tx_head + count) % UART_TX_SIZE;
    uart->tx_count -= count;
    return count;
}

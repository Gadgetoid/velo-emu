#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifndef UART_WIRE_SIZE
#define UART_WIRE_SIZE 65536
#endif
#ifndef UART_TX_SIZE
#define UART_TX_SIZE   65536
#endif

typedef struct {
    uint32_t ctl1;
    uint32_t baud_divisor;
    uint32_t dma_buffer;
    uint32_t dma_length;
    uint32_t dma_count;
    bool     dma_armed;
    uint64_t rx_next;
    uint8_t  wire[UART_WIRE_SIZE];
    uint32_t wire_head, wire_count;
    uint8_t  tx[UART_TX_SIZE];
    uint32_t tx_head, tx_count;
} uart_t;

typedef struct {
    uart_t  *state;
    uint8_t *dram;
    uint32_t dram_mask;
    void    *context;
    void   (*raise)(void *context, uint32_t status2_bits);
} uart_port_t;

uint32_t uart_read(uart_port_t *port, uint32_t offset);
void     uart_write(uart_port_t *port, uint32_t offset, uint32_t value, uint64_t now);
void     uart_receive(uart_port_t *port, const uint8_t *data, uint32_t length, uint64_t now);
uint64_t uart_next_event(const uart_port_t *port);
void     uart_event(uart_port_t *port, uint64_t now);
uint32_t uart_take_tx(uart_port_t *port, uint8_t *out, uint32_t max);
uint32_t uart_baud(const uart_port_t *port);
uint32_t uart_space(const uart_port_t *port);
void     uart_sanitize(uart_t *uart);

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/machine.h"
#include "net/net_gateway.h"

#define SERIAL_LINK_QUEUE 65536
#define SERIAL_LINK_PORT_NAME 64

typedef enum { SERIAL_OFF, SERIAL_NETWORK, SERIAL_PTY, SERIAL_DEVICE, SERIAL_TCP } serial_mode_t;

typedef struct {
    serial_mode_t mode;
    net_gateway_t *gateway;
    net_gateway_log_fn log;
    net_gateway_options_t options;
    bool dtr;
    int fd;
    int pty_slave;
    int listener;
    int tcp_port;
    char name[256];
    uint32_t baud;
    uint8_t queue[SERIAL_LINK_QUEUE];
    size_t queued;
} serial_link_t;

void serial_link_init(serial_link_t *link, net_gateway_log_fn log);
const char *serial_link_open(serial_link_t *link, serial_mode_t mode, const char *device);
void serial_link_close(serial_link_t *link);
void serial_link_pump(serial_link_t *link, machine_t *machine);
bool serial_link_attached(const serial_link_t *link);
int serial_link_ports(char ports[][SERIAL_LINK_PORT_NAME], int max);

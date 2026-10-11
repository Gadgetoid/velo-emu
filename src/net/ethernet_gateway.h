#pragma once
#include <stddef.h>
#include <stdint.h>

#include "net/net_gateway.h"

typedef struct ethernet_gateway ethernet_gateway_t;

ethernet_gateway_t *ethernet_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options);
void                ethernet_gateway_destroy(ethernet_gateway_t *gateway);
void                ethernet_gateway_from_guest(ethernet_gateway_t *gateway, const uint8_t *frame, size_t length);
size_t              ethernet_gateway_to_guest(ethernet_gateway_t *gateway, uint8_t *frame, size_t max);
void                ethernet_gateway_poll(ethernet_gateway_t *gateway, uint64_t guest_ms);

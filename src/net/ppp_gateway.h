#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net/net_gateway.h"

typedef struct ppp_gateway ppp_gateway_t;

ppp_gateway_t *ppp_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options);
void           ppp_gateway_destroy(ppp_gateway_t *gateway);
void           ppp_gateway_reset(ppp_gateway_t *gateway);
void           ppp_gateway_from_guest(ppp_gateway_t *gateway, const uint8_t *data, size_t length);
size_t         ppp_gateway_to_guest(ppp_gateway_t *gateway, uint8_t *out, size_t max);
void           ppp_gateway_poll(ppp_gateway_t *gateway, uint64_t guest_ms);
bool           ppp_gateway_online(const ppp_gateway_t *gateway);
net_gateway_t *ppp_gateway_network(ppp_gateway_t *gateway);

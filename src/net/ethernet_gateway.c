#include "net/ethernet_gateway.h"

#include <stdlib.h>
#include <string.h>

#define QUEUE 64

struct ethernet_gateway {
    net_gateway_t *network;
    uint8_t queued[QUEUE][NET_GATEWAY_FRAME_MAX];
    uint16_t queued_length[QUEUE];
    size_t queued_head, queued_count;
};

static void from_network(void *context, const uint8_t *frame, size_t length) {
    ethernet_gateway_t *gateway = context;
    if (length > NET_GATEWAY_FRAME_MAX || gateway->queued_count == QUEUE) return;
    size_t slot = (gateway->queued_head + gateway->queued_count) % QUEUE;
    memcpy(gateway->queued[slot], frame, length);
    gateway->queued_length[slot] = (uint16_t)length;
    gateway->queued_count++;
}

ethernet_gateway_t *ethernet_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options) {
    ethernet_gateway_t *gateway = calloc(1, sizeof *gateway);
    if (!gateway) return NULL;
    gateway->network = net_gateway_create(log, options, from_network, gateway);
    if (!gateway->network) {
        free(gateway);
        return NULL;
    }
    return gateway;
}

void ethernet_gateway_destroy(ethernet_gateway_t *gateway) {
    if (!gateway) return;
    net_gateway_destroy(gateway->network);
    free(gateway);
}

void ethernet_gateway_from_guest(ethernet_gateway_t *gateway, const uint8_t *frame, size_t length) {
    net_gateway_input(gateway->network, frame, length);
}

size_t ethernet_gateway_to_guest(ethernet_gateway_t *gateway, uint8_t *frame, size_t max) {
    if (!gateway->queued_count) return 0;
    size_t length = gateway->queued_length[gateway->queued_head];
    if (length > max) length = max;
    memcpy(frame, gateway->queued[gateway->queued_head], length);
    gateway->queued_head = (gateway->queued_head + 1) % QUEUE;
    gateway->queued_count--;
    return length;
}

void ethernet_gateway_poll(ethernet_gateway_t *gateway, uint64_t guest_ms) {
    net_gateway_poll(gateway->network, guest_ms);
}

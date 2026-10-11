#include "net/net_gateway.h"

#include <stddef.h>

const uint8_t NET_GATEWAY_GUEST_IP[4] = { 10, 0, 2, 15 };
const uint8_t NET_GATEWAY_HOST_IP[4] = { 10, 0, 2, 2 };
const uint8_t NET_GATEWAY_DNS_IP[4] = { 10, 0, 2, 3 };
const uint8_t NET_GATEWAY_HOST_MAC[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

bool           net_gateway_available(void) {
    return false;
}
net_gateway_t *net_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options, net_gateway_sink_fn sink, void *context) {
    (void)log; (void)options; (void)sink; (void)context; return NULL;
}
void           net_gateway_destroy(net_gateway_t *gateway) {
    (void)gateway;
}
void           net_gateway_input(net_gateway_t *gateway, const uint8_t *frame, size_t length) {
    (void)gateway; (void)frame; (void)length;
}
void           net_gateway_poll(net_gateway_t *gateway, uint64_t guest_ms) {
    (void)gateway; (void)guest_ms;
}
bool           net_gateway_take_desktop_connected(net_gateway_t *gateway) {
    (void)gateway; return false;
}

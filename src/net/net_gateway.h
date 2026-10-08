#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NET_GATEWAY_DEFAULT_USER_AGENT "Lynx/2.9.2 libwww-FM/2.14 SSL-MM/1.4.1 OpenSSL/3.0.13"

typedef struct net_gateway net_gateway_t;

typedef struct {
    const char *user_agent;
    const char *rapi_socket;
    int rapi_port;
} net_gateway_options_t;

typedef void (*net_gateway_log_fn)(const char *message);

bool     net_gateway_available(void);
net_gateway_t *net_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options);
void     net_gateway_destroy(net_gateway_t *gateway);
void     net_gateway_reset(net_gateway_t *gateway);
void     net_gateway_from_guest(net_gateway_t *gateway, const uint8_t *data, size_t length);
size_t   net_gateway_to_guest(net_gateway_t *gateway, uint8_t *out, size_t max);
void     net_gateway_poll(net_gateway_t *gateway, uint64_t guest_ms);
bool     net_gateway_online(const net_gateway_t *gateway);
bool     net_gateway_take_desktop_connected(net_gateway_t *gateway);
bool     net_gateway_socket_path(char *path, size_t size, const char *name);

#include "net/net_gateway.h"
#include "net/web_proxy.h"
#include "util/socket_path.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libslirp.h>

#define MAX_POLL    64
#define MAX_TIMERS  4

#define ETH_HEADER 14
#define ETH_IPV4   0x0800

#define PROXY_PORT 8080
#define DESKTOP_PORT 5679
#define DESKTOP_CLIENTS 4
#define RAPI_RELAYS     4
#define RELAY_BUFFER    16384
#define DCCM_PING 0x12345678u
#define DCCM_PACKET_MAX 512
#define DCCM_PING_MS 1000
#define RAPI_PORT 990
#define SOCKET_NAME_MAX 64

#if !SLIRP_CHECK_VERSION(4, 9, 0)
typedef int slirp_os_socket;
#endif
#if !SLIRP_CHECK_VERSION(4, 8, 0)
typedef ssize_t slirp_ssize_t;
#endif

const uint8_t NET_GATEWAY_GUEST_IP[4] = { 10, 0, 2, 15 };
const uint8_t NET_GATEWAY_HOST_IP[4] = { 10, 0, 2, 2 };
const uint8_t NET_GATEWAY_DNS_IP[4] = { 10, 0, 2, 3 };
const uint8_t NET_GATEWAY_HOST_MAC[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 };

static const uint8_t desktop_alias_ip[4] = { 10, 0, 2, 5 };
static const char proxy_address[] = "10.0.2.4";

typedef struct {
    int fd;
    uint8_t buffer[DCCM_PACKET_MAX + 4];
    size_t length;
    int64_t next_ping_ms;
} desktop_client_t;

typedef struct {
    int client, upstream;
    uint8_t to_upstream[RELAY_BUFFER], to_client[RELAY_BUFFER];
    size_t upstream_length, client_length;
} rapi_relay_t;

typedef struct {
    bool used;
    int64_t expire_ms;
    SlirpTimerId id;
    void    *cb_opaque;
} net_gateway_timer_t;

struct net_gateway {
    net_gateway_log_fn log;
    net_gateway_sink_fn sink;
    void    *context;
    Slirp   *slirp;
    web_proxy_t *proxy;
    int instance;
    int desktop_listener;
    bool desktop_aliased;
    desktop_client_t desktop_clients[DESKTOP_CLIENTS];
    struct sockaddr_un desktop_address;
    bool desktop_connected;
    char rapi_socket[sizeof ((struct sockaddr_un *)0)->sun_path];
    int rapi_listener;
    rapi_relay_t rapi_relays[RAPI_RELAYS];
    struct pollfd fds[MAX_POLL];
    int fd_count;
    net_gateway_timer_t timers[MAX_TIMERS];
};

static void gateway_log(net_gateway_t *gateway, const char *format, ...) {
    if (!gateway->log) return;
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    gateway->log(message);
}

static void adjust_checksum(uint8_t *checksum, const uint8_t *old_address, const uint8_t *new_address) {
    uint32_t sum = (uint16_t) ~(checksum[0] << 8 | checksum[1]);
    for (int i = 0; i < 4; i += 2) {
        sum += (uint16_t) ~(old_address[i] << 8 | old_address[i + 1]);
        sum += (uint16_t)(new_address[i] << 8 | new_address[i + 1]);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~sum & 0xFFFF;
    checksum[0] = (uint8_t)(sum >> 8);
    checksum[1] = (uint8_t)sum;
}

static void retarget_desktop(uint8_t *ip, size_t length, bool outbound) {
    if (length < 20 || (ip[0] >> 4) != 4 || ip[9] != 6) return;
    size_t header = (size_t)(ip[0] & 15) * 4;
    if (length < header + 18) return;
    uint8_t *address = ip + (outbound ? 16 : 12);
    const uint8_t *port = ip + header + (outbound ? 2 : 0);
    const uint8_t *from = outbound ? NET_GATEWAY_HOST_IP : desktop_alias_ip, *to = outbound ? desktop_alias_ip : NET_GATEWAY_HOST_IP;
    if (memcmp(address, from, 4) || ((port[0] << 8) | port[1]) != DESKTOP_PORT) return;
    adjust_checksum(ip + 10, address, to);
    adjust_checksum(ip + header + 16, address, to);
    memcpy(address, to, 4);
}

static bool is_ipv4(const uint8_t *frame, size_t length) {
    return length > ETH_HEADER && frame[12] == (ETH_IPV4 >> 8) && frame[13] == (ETH_IPV4 & 0xFF);
}

static slirp_ssize_t slirp_send_packet(const void *buffer, size_t length, void *opaque) {
    net_gateway_t *gateway = opaque;
    if (length > NET_GATEWAY_FRAME_MAX) return (slirp_ssize_t)length;
    if (gateway->desktop_aliased && is_ipv4(buffer, length)) {
        uint8_t frame[NET_GATEWAY_FRAME_MAX];
        memcpy(frame, buffer, length);
        retarget_desktop(frame + ETH_HEADER, length - ETH_HEADER, false);
        gateway->sink(gateway->context, frame, length);
    } else {
        gateway->sink(gateway->context, buffer, length);
    }
    return (slirp_ssize_t)length;
}

static void slirp_guest_error(const char *message, void *opaque) {
    gateway_log(opaque, "slirp: %s\n", message);
}

static int64_t slirp_clock_ns(void *opaque) {
    (void)opaque;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void *slirp_timer_new_opaque(SlirpTimerId id, void *cb_opaque, void *opaque) {
    net_gateway_t *gateway = opaque;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!gateway->timers[i].used) {
            gateway->timers[i] = (net_gateway_timer_t){ true, -1, id, cb_opaque };
            return &gateway->timers[i];
        }
    }
    return NULL;
}

static void slirp_timer_free(void *timer, void *opaque) {
    (void)opaque;
    if (timer) ((net_gateway_timer_t *)timer)->used = false;
}

static void slirp_timer_mod(void *timer, int64_t expire_ms, void *opaque) {
    (void)opaque;
    if (timer) ((net_gateway_timer_t *)timer)->expire_ms = expire_ms;
}

static void slirp_register_socket(slirp_os_socket socket, void *opaque) {
    (void)socket; (void)opaque;
}
static void slirp_unregister_socket(slirp_os_socket socket, void *opaque) {
    (void)socket; (void)opaque;
}
static void slirp_notify(void *opaque) {
    (void)opaque;
}

static const SlirpCb callbacks = {
    .send_packet = slirp_send_packet,
    .guest_error = slirp_guest_error,
    .clock_get_ns = slirp_clock_ns,
    .timer_free = slirp_timer_free,
    .timer_mod = slirp_timer_mod,
    .notify = slirp_notify,
    .timer_new_opaque = slirp_timer_new_opaque,
#if SLIRP_CHECK_VERSION(4, 9, 0)
    .register_poll_socket = slirp_register_socket,
    .unregister_poll_socket = slirp_unregister_socket,
#else
    .register_poll_fd = slirp_register_socket,
    .unregister_poll_fd = slirp_unregister_socket,
#endif
};

static void start_proxy(net_gateway_t *gateway, const char *user_agent) {
#if SLIRP_CHECK_VERSION(4, 7, 0)
    gateway->proxy = web_proxy_start(gateway->log, user_agent);
    if (!gateway->proxy) return;
    struct in_addr address;
    inet_pton(AF_INET, proxy_address, &address);
    if (slirp_add_unix(gateway->slirp, web_proxy_socket_path(gateway->proxy), &address, PROXY_PORT) < 0) {
        gateway_log(gateway, "proxy: could not forward port %d\n", PROXY_PORT);
        web_proxy_stop(gateway->proxy);
        gateway->proxy = NULL;
        return;
    }
    gateway_log(gateway, "proxy: web proxy at %s:%d\n", proxy_address, PROXY_PORT);
#else
    (void)gateway;
    (void)user_agent;
#endif
}

bool net_gateway_available(void) {
    return true;
}

static bool socket_name(const net_gateway_t *gateway, char *path, size_t size, const char *name) {
    char numbered[SOCKET_NAME_MAX];
    snprintf(numbered, sizeof numbered, "%s-%d", name, gateway->instance);
    return socket_path(path, size, numbered);
}

static void start_desktop(net_gateway_t *gateway) {
    gateway->desktop_listener = -1;
    for (int i = 0; i < DESKTOP_CLIENTS; i++) gateway->desktop_clients[i].fd = -1;
#if SLIRP_CHECK_VERSION(4, 7, 0)
    struct sockaddr_un *address = &gateway->desktop_address;
    address->sun_family = AF_UNIX;
    if (!socket_name(gateway, address->sun_path, sizeof address->sun_path, "velo-desktop")) {
        gateway_log(gateway, "desktop: no usable socket path\n");
        return;
    }
    unlink(address->sun_path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)address, sizeof *address) != 0 || listen(listener, DESKTOP_CLIENTS) != 0) {
        gateway_log(gateway, "desktop: could not listen on %s: %s\n", address->sun_path, strerror(errno));
        if (listener >= 0) close(listener);
        return;
    }
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);
    struct in_addr host;
    memcpy(&host, NET_GATEWAY_HOST_IP, sizeof host);
    if (slirp_add_unix(gateway->slirp, address->sun_path, &host, DESKTOP_PORT) < 0) {
        memcpy(&host, desktop_alias_ip, sizeof host);
        if (slirp_add_unix(gateway->slirp, address->sun_path, &host, DESKTOP_PORT) < 0) {
            gateway_log(gateway, "desktop: could not forward port %d\n", DESKTOP_PORT);
            close(listener);
            unlink(address->sun_path);
            return;
        }
        gateway->desktop_aliased = true;
    }
    gateway->desktop_listener = listener;
#endif
}

static void stop_desktop(net_gateway_t *gateway) {
    for (int i = 0; i < DESKTOP_CLIENTS; i++) {
        if (gateway->desktop_clients[i].fd >= 0) close(gateway->desktop_clients[i].fd);
    }
    if (gateway->desktop_listener < 0) return;
    close(gateway->desktop_listener);
    unlink(gateway->desktop_address.sun_path);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void send_ping(desktop_client_t *client) {
    uint8_t ping[4] = { 0x78, 0x56, 0x34, 0x12 };
    ssize_t sent = send(client->fd, ping, sizeof ping, 0);
    (void)sent;
}

static void log_device(net_gateway_t *gateway, const uint8_t *packet, uint32_t length) {
    char name[64] = "";
    uint32_t offset = length >= 0x1C ? read_u32(packet + 0x18) : 0;
    for (size_t i = 0; offset + i * 2 + 1 < length && i + 1 < sizeof name; i++) {
        uint16_t unit = (uint16_t)(packet[offset + i * 2] | packet[offset + i * 2 + 1] << 8);
        if (!unit) break;
        name[i] = unit < 0x80 ? (char)unit : '?';
        name[i + 1] = 0;
    }
    gateway_log(gateway, "desktop: %s, Windows CE %u.x\n", name, length >= 8 ? (unsigned)(packet[4] | packet[5] << 8) : 0);
}

static void handle_packets(net_gateway_t *gateway, desktop_client_t *client, int64_t now_ms) {
    while (client->length >= 4) {
        uint32_t header = read_u32(client->buffer);
        size_t used = 4;
        if (header != 0 && header != DCCM_PING) {
            if (header >= DCCM_PACKET_MAX) {
                gateway_log(gateway, "desktop: the Velo asked for a password, which isn't supported\n");
            } else {
                if (client->length < 4 + header) return;
                log_device(gateway, client->buffer + 4, header);
                send_ping(client);
                client->next_ping_ms = now_ms + DCCM_PING_MS;
                used += header;
            }
        }
        memmove(client->buffer, client->buffer + used, client->length - used);
        client->length -= used;
    }
}

static void poll_desktop(net_gateway_t *gateway, int64_t now_ms) {
    if (gateway->desktop_listener < 0) return;
    int fd;
    while ((fd = accept(gateway->desktop_listener, NULL, NULL)) >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        int slot = 0;
        while (slot < DESKTOP_CLIENTS && gateway->desktop_clients[slot].fd >= 0) slot++;
        if (slot == DESKTOP_CLIENTS) {
            close(fd);
            continue;
        }
        gateway->desktop_clients[slot] = (desktop_client_t){ .fd = fd };
        gateway->desktop_connected = true;
        gateway_log(gateway, "desktop: connection from the Velo\n");
    }
    for (int i = 0; i < DESKTOP_CLIENTS; i++) {
        desktop_client_t *client = &gateway->desktop_clients[i];
        if (client->fd < 0) continue;
        ssize_t got;
        while ((got = read(client->fd, client->buffer + client->length, sizeof client->buffer - client->length)) > 0) {
            client->length += (size_t)got;
            handle_packets(gateway, client, now_ms);
            if (client->length == sizeof client->buffer) client->length = 0;
        }
        if (got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(client->fd);
            client->fd = -1;
            continue;
        }
        if (client->next_ping_ms && now_ms >= client->next_ping_ms) {
            send_ping(client);
            client->next_ping_ms = now_ms + DCCM_PING_MS;
        }
    }
}

static void start_rapi(net_gateway_t *gateway, const char *path) {
#if SLIRP_CHECK_VERSION(4, 7, 0)
    if (!path || !*path) return;
    struct sockaddr_un host = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof host.sun_path) {
        gateway_log(gateway, "rapi: socket path too long: %s\n", path);
        return;
    }
    snprintf(host.sun_path, sizeof host.sun_path, "%s", path);
    struct sockaddr_in guest = { .sin_family = AF_INET, .sin_port = htons(RAPI_PORT) };
    memcpy(&guest.sin_addr, NET_GATEWAY_GUEST_IP, sizeof guest.sin_addr);
    unlink(path);
    if (slirp_add_hostxfwd(gateway->slirp, (struct sockaddr *)&host, sizeof host, (struct sockaddr *)&guest, sizeof guest, 0) < 0) {
        gateway_log(gateway, "rapi: could not listen on %s\n", path);
        return;
    }
    snprintf(gateway->rapi_socket, sizeof gateway->rapi_socket, "%s", path);
    gateway_log(gateway, "rapi: %s\n", path);
#else
    (void)gateway;
    (void)path;
#endif
}

static void start_rapi_port(net_gateway_t *gateway, int port) {
    gateway->rapi_listener = -1;
    for (int i = 0; i < RAPI_RELAYS; i++) gateway->rapi_relays[i].client = gateway->rapi_relays[i].upstream = -1;
    if (port <= 0) return;
    if (!gateway->rapi_socket[0]) {
        char path[sizeof gateway->rapi_socket];
        if (socket_name(gateway, path, sizeof path, "velo-rapi-relay")) start_rapi(gateway, path);
    }
    if (!gateway->rapi_socket[0]) return;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    if (listener >= 0) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof address) != 0 || listen(listener, RAPI_RELAYS) != 0) {
        if (listener >= 0) close(listener);
        gateway_log(gateway, "rapi: could not listen on port %d: %s\n", port, strerror(errno));
        return;
    }
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);
    gateway->rapi_listener = listener;
    gateway_log(gateway, "rapi: listening on all interfaces, port %d\n", port);
}

static void close_relay(rapi_relay_t *relay) {
    if (relay->client >= 0) close(relay->client);
    if (relay->upstream >= 0) close(relay->upstream);
    relay->client = relay->upstream = -1;
    relay->upstream_length = relay->client_length = 0;
}

static bool relay_pump(int from, int to, uint8_t *buffer, size_t *length) {
    if (*length < RELAY_BUFFER) {
        ssize_t got = recv(from, buffer + *length, RELAY_BUFFER - *length, MSG_DONTWAIT);
        if (got == 0) return false;
        if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (got > 0) *length += (size_t)got;
    }
    if (*length) {
        ssize_t sent = send(to, buffer, *length, MSG_DONTWAIT);
        if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return false;
        if (sent > 0) {
            memmove(buffer, buffer + sent, *length - (size_t)sent);
            *length -= (size_t)sent;
        }
    }
    return true;
}

static void poll_rapi_relays(net_gateway_t *gateway) {
    if (gateway->rapi_listener < 0) return;
    int client;
    while ((client = accept(gateway->rapi_listener, NULL, NULL)) >= 0) {
        rapi_relay_t *relay = NULL;
        for (int i = 0; i < RAPI_RELAYS && !relay; i++) {
            if (gateway->rapi_relays[i].client < 0) relay = &gateway->rapi_relays[i];
        }
        struct sockaddr_un address = { .sun_family = AF_UNIX };
        snprintf(address.sun_path, sizeof address.sun_path, "%s", gateway->rapi_socket);
        int upstream = relay ? socket(AF_UNIX, SOCK_STREAM, 0) : -1;
        if (upstream < 0 || connect(upstream, (struct sockaddr *)&address, sizeof address) != 0) {
            if (upstream >= 0) close(upstream);
            close(client);
            continue;
        }
        fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
        fcntl(upstream, F_SETFL, fcntl(upstream, F_GETFL) | O_NONBLOCK);
        relay->client = client;
        relay->upstream = upstream;
    }
    for (int i = 0; i < RAPI_RELAYS; i++) {
        rapi_relay_t *relay = &gateway->rapi_relays[i];
        if (relay->client < 0) continue;
        if (!relay_pump(relay->client, relay->upstream, relay->to_upstream, &relay->upstream_length) ||
            !relay_pump(relay->upstream, relay->client, relay->to_client, &relay->client_length)) close_relay(relay);
    }
}

static void stop_rapi_relays(net_gateway_t *gateway) {
    if (gateway->rapi_listener >= 0) close(gateway->rapi_listener);
    gateway->rapi_listener = -1;
    for (int i = 0; i < RAPI_RELAYS; i++) close_relay(&gateway->rapi_relays[i]);
}

net_gateway_t *net_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options, net_gateway_sink_fn sink, void *context) {
    static int instances;
    signal(SIGPIPE, SIG_IGN);
    net_gateway_t *gateway = calloc(1, sizeof *gateway);
    if (!gateway) return NULL;
    gateway->log = log;
    gateway->sink = sink;
    gateway->context = context;
    gateway->instance = instances++;
    gateway->desktop_listener = gateway->rapi_listener = -1;
    for (int i = 0; i < DESKTOP_CLIENTS; i++) gateway->desktop_clients[i].fd = -1;
    for (int i = 0; i < RAPI_RELAYS; i++) gateway->rapi_relays[i].client = gateway->rapi_relays[i].upstream = -1;
    SlirpConfig config = { 0 };
    config.version = SLIRP_CONFIG_VERSION_MAX < 6 ? SLIRP_CONFIG_VERSION_MAX : 6;
    config.in_enabled = true;
    inet_pton(AF_INET, "10.0.2.0", &config.vnetwork);
    inet_pton(AF_INET, "255.255.255.0", &config.vnetmask);
    inet_pton(AF_INET, "10.0.2.2", &config.vhost);
    inet_pton(AF_INET, "10.0.2.15", &config.vdhcp_start);
    inet_pton(AF_INET, "10.0.2.3", &config.vnameserver);
    config.vhostname = "velo-host";
    config.if_mtu = 1500;
    config.if_mru = 1500;
    gateway->slirp = slirp_new(&config, &callbacks, gateway);
    if (!gateway->slirp) {
        free(gateway);
        return NULL;
    }
    start_proxy(gateway, options ? options->user_agent : NULL);
    start_desktop(gateway);
    start_rapi(gateway, options ? options->rapi_socket : NULL);
    start_rapi_port(gateway, options ? options->rapi_port : 0);
    return gateway;
}

void net_gateway_destroy(net_gateway_t *gateway) {
    if (!gateway) return;
    slirp_cleanup(gateway->slirp);
    web_proxy_stop(gateway->proxy);
    stop_desktop(gateway);
    stop_rapi_relays(gateway);
    if (gateway->rapi_socket[0]) unlink(gateway->rapi_socket);
    free(gateway);
}

void net_gateway_input(net_gateway_t *gateway, const uint8_t *frame, size_t length) {
    if (length < ETH_HEADER || length > NET_GATEWAY_FRAME_MAX) return;
    if (gateway->desktop_aliased && is_ipv4(frame, length)) {
        uint8_t copy[NET_GATEWAY_FRAME_MAX];
        memcpy(copy, frame, length);
        retarget_desktop(copy + ETH_HEADER, length - ETH_HEADER, true);
        slirp_input(gateway->slirp, copy, (int)length);
        return;
    }
    slirp_input(gateway->slirp, frame, (int)length);
}

static int add_poll(slirp_os_socket fd, int events, void *opaque) {
    net_gateway_t *gateway = opaque;
    if (gateway->fd_count == MAX_POLL) return -1;
    struct pollfd *entry = &gateway->fds[gateway->fd_count];
    entry->fd = fd;
    entry->events = 0;
    if (events & SLIRP_POLL_IN) entry->events |= POLLIN;
    if (events & SLIRP_POLL_OUT) entry->events |= POLLOUT;
    if (events & SLIRP_POLL_PRI) entry->events |= POLLPRI;
    entry->revents = 0;
    return gateway->fd_count++;
}

static int get_revents(int index, void *opaque) {
    net_gateway_t *gateway = opaque;
    if (index < 0 || index >= gateway->fd_count) return 0;
    short revents = gateway->fds[index].revents;
    int events = 0;
    if (revents & POLLIN) events |= SLIRP_POLL_IN;
    if (revents & POLLOUT) events |= SLIRP_POLL_OUT;
    if (revents & POLLPRI) events |= SLIRP_POLL_PRI;
    if (revents & POLLERR) events |= SLIRP_POLL_ERR;
    if (revents & POLLHUP) events |= SLIRP_POLL_HUP;
    return events;
}

void net_gateway_poll(net_gateway_t *gateway, uint64_t guest_ms) {
    web_proxy_poll(gateway->proxy);
    poll_desktop(gateway, (int64_t)guest_ms);
    poll_rapi_relays(gateway);
    int64_t now_ms = slirp_clock_ns(gateway) / 1000000;
    for (int i = 0; i < MAX_TIMERS; i++) {
        net_gateway_timer_t *timer = &gateway->timers[i];
        if (timer->used && timer->expire_ms >= 0 && timer->expire_ms <= now_ms) {
            timer->expire_ms = -1;
            slirp_handle_timer(gateway->slirp, timer->id, timer->cb_opaque);
        }
    }
    uint32_t timeout = 0;
    gateway->fd_count = 0;
#if SLIRP_CHECK_VERSION(4, 9, 0)
    slirp_pollfds_fill_socket(gateway->slirp, &timeout, add_poll, gateway);
#else
    slirp_pollfds_fill(gateway->slirp, &timeout, add_poll, gateway);
#endif
    int result = gateway->fd_count ? poll(gateway->fds, (nfds_t)gateway->fd_count, 0) : 0;
    slirp_pollfds_poll(gateway->slirp, result < 0, get_revents, gateway);
}

bool net_gateway_take_desktop_connected(net_gateway_t *gateway) {
    bool connected = gateway->desktop_connected;
    gateway->desktop_connected = false;
    return connected;
}

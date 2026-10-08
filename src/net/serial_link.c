#include "net/serial_link.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define DEVICE_DEFAULT_SPEED B19200

void serial_link_init(serial_link_t *link, net_gateway_log_fn log) {
    memset(link, 0, sizeof *link);
    link->log = log;
    link->options = (net_gateway_options_t){ NET_GATEWAY_DEFAULT_USER_AGENT, NULL, 0 };
    link->fd = -1;
    link->pty_slave = -1;
    link->listener = -1;
}

void serial_link_close(serial_link_t *link) {
    if (link->gateway) net_gateway_destroy(link->gateway);
    if (link->fd >= 0) close(link->fd);
    if (link->pty_slave >= 0) close(link->pty_slave);
    if (link->listener >= 0) close(link->listener);
    link->gateway = NULL;
    link->fd = -1;
    link->pty_slave = -1;
    link->listener = -1;
    link->queued = 0;
    link->baud = 0;
    link->dtr = false;
    link->mode = SERIAL_OFF;
}

static const char *open_pty(serial_link_t *link) {
    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0 || grantpt(fd) != 0 || unlockpt(fd) != 0) {
        if (fd >= 0) close(fd);
        return "could not open a pseudo-terminal";
    }
    snprintf(link->name, sizeof link->name, "%s", ptsname(fd));
    int slave = open(link->name, O_RDWR | O_NOCTTY);
    struct termios settings;
    if (slave >= 0 && tcgetattr(slave, &settings) == 0) {
        cfmakeraw(&settings);
        tcsetattr(slave, TCSANOW, &settings);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    link->fd = fd;
    link->pty_slave = slave;
    return NULL;
}

static const char *open_device(serial_link_t *link, const char *device) {
    if (!device || !device[0]) return "choose a host serial port first";
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    struct termios settings;
    if (fd < 0 || tcgetattr(fd, &settings) != 0) {
        if (fd >= 0) close(fd);
        snprintf(link->name, sizeof link->name, "could not open %s", device);
        return link->name;
    }
    cfmakeraw(&settings);
    settings.c_cflag |= CLOCAL | CREAD;
    settings.c_cflag &= ~(tcflag_t)CRTSCTS;
    cfsetispeed(&settings, DEVICE_DEFAULT_SPEED);
    cfsetospeed(&settings, DEVICE_DEFAULT_SPEED);
    tcsetattr(fd, TCSANOW, &settings);
    link->fd = fd;
    snprintf(link->name, sizeof link->name, "%s", device);
    return NULL;
}

static const char *open_tcp(serial_link_t *link) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int enabled = 1;
    if (listener >= 0) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled);
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_port = htons((uint16_t)link->tcp_port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (listener < 0 || link->tcp_port <= 0 || link->tcp_port > 65535 || bind(listener, (struct sockaddr *)&address, sizeof address) != 0 || listen(listener, 1) != 0) {
        snprintf(link->name, sizeof link->name, "could not listen on port %d: %s", link->tcp_port, strerror(errno));
        if (listener >= 0) close(listener);
        return link->name;
    }
    fcntl(listener, F_SETFL, fcntl(listener, F_GETFL) | O_NONBLOCK);
    link->listener = listener;
    snprintf(link->name, sizeof link->name, "port %d", link->tcp_port);
    return NULL;
}

const char *serial_link_open(serial_link_t *link, serial_mode_t mode, const char *device) {
    serial_link_close(link);
    const char *failure = NULL;
    if (mode == SERIAL_NETWORK) {
        link->gateway = net_gateway_create(link->log, &link->options);
        if (!link->gateway) failure = net_gateway_available() ? "cannot start the network" : "this build has no network (libslirp)";
    } else if (mode == SERIAL_PTY) {
        failure = open_pty(link);
    } else if (mode == SERIAL_DEVICE) {
        failure = open_device(link, device);
    } else if (mode == SERIAL_TCP) {
        failure = open_tcp(link);
    }
    if (failure) {
        serial_link_close(link);
        return failure;
    }
    link->mode = mode;
    return NULL;
}

static speed_t speed_for(uint32_t baud) {
    static const struct { uint32_t baud; speed_t speed; } speeds[] = {
        { 300, B300 }, { 1200, B1200 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
        { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 }, { 115200, B115200 },
    };
    speed_t best = DEVICE_DEFAULT_SPEED;
    uint32_t best_error = UINT32_MAX;
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++) {
        uint32_t error = speeds[i].baud > baud ? speeds[i].baud - baud : baud - speeds[i].baud;
        if (error < best_error) {
            best_error = error;
            best = speeds[i].speed;
        }
    }
    return best;
}

static void follow_baud(serial_link_t *link, machine_t *machine) {
    uint32_t baud = machine_serial_baud(machine);
    if (link->mode != SERIAL_DEVICE || !baud || baud == link->baud) return;
    struct termios settings;
    if (tcgetattr(link->fd, &settings) != 0) return;
    cfsetispeed(&settings, speed_for(baud));
    cfsetospeed(&settings, speed_for(baud));
    tcsetattr(link->fd, TCSANOW, &settings);
    link->baud = baud;
    if (link->log) {
        char message[320];
        snprintf(message, sizeof message, "serial: %s at %u baud\n", link->name, baud);
        link->log(message);
    }
}

static void pump_network(serial_link_t *link, machine_t *machine) {
    uint8_t buffer[4096];
    size_t count;
    bool dtr = machine_serial_dtr(machine);
    if (dtr && !link->dtr) net_gateway_reset(link->gateway);
    link->dtr = dtr;
    while ((count = machine_serial_take(machine, buffer, sizeof buffer)) > 0) net_gateway_from_guest(link->gateway, buffer, count);
    net_gateway_poll(link->gateway, machine_cycles(machine) / (MACHINE_CLOCK_HZ / 1000));
    size_t space = machine_serial_space(machine);
    count = net_gateway_to_guest(link->gateway, buffer, space < sizeof buffer ? space : sizeof buffer);
    machine_serial_send(machine, buffer, count);
}

static void link_log(serial_link_t *link, const char *message) {
    if (link->log) link->log(message);
}

static void drop_client(serial_link_t *link) {
    close(link->fd);
    link->fd = -1;
    link->queued = 0;
    link_log(link, "serial: TCP client disconnected\n");
}

static void accept_client(serial_link_t *link) {
    int client = accept(link->listener, NULL, NULL);
    if (client < 0) return;
    if (link->fd >= 0) {
        close(client);
        return;
    }
    int enabled = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof enabled);
    fcntl(client, F_SETFL, fcntl(client, F_GETFL) | O_NONBLOCK);
    link->fd = client;
    link->queued = 0;
    link_log(link, "serial: TCP client connected\n");
}

static bool would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static ssize_t send_bytes(serial_link_t *link, const uint8_t *data, size_t length) {
    if (link->mode == SERIAL_TCP) return send(link->fd, data, length, SEND_FLAGS);
    return write(link->fd, data, length);
}

static void pump_host(serial_link_t *link, machine_t *machine) {
    uint8_t buffer[4096];
    size_t count;
    follow_baud(link, machine);
    while (link->queued < sizeof link->queue && (count = machine_serial_take(machine, link->queue + link->queued, sizeof link->queue - link->queued)) > 0) {
        link->queued += count;
    }
    if (link->queued) {
        ssize_t written = send_bytes(link, link->queue, link->queued);
        if (written < 0 && link->mode == SERIAL_TCP && !would_block()) {
            drop_client(link);
            return;
        }
        if (written > 0) {
            memmove(link->queue, link->queue + written, link->queued - (size_t)written);
            link->queued -= (size_t)written;
        }
    }
    size_t space = machine_serial_space(machine);
    if (space > sizeof buffer) space = sizeof buffer;
    if (!space) return;
    ssize_t got = read(link->fd, buffer, space);
    if (got > 0) machine_serial_send(machine, buffer, (size_t)got);
    else if (link->mode == SERIAL_TCP && (got == 0 || !would_block())) drop_client(link);
}

static void pump_tcp(serial_link_t *link, machine_t *machine) {
    accept_client(link);
    if (link->fd >= 0) {
        pump_host(link, machine);
        return;
    }
    uint8_t discard[4096];
    while (machine_serial_take(machine, discard, sizeof discard) > 0) {}
}

void serial_link_pump(serial_link_t *link, machine_t *machine) {
    if (link->mode == SERIAL_NETWORK && link->gateway) pump_network(link, machine);
    else if ((link->mode == SERIAL_PTY || link->mode == SERIAL_DEVICE) && link->fd >= 0) pump_host(link, machine);
    else if (link->mode == SERIAL_TCP && link->listener >= 0) pump_tcp(link, machine);
}

bool serial_link_attached(const serial_link_t *link) {
    if (link->mode == SERIAL_TCP) return link->fd >= 0;
    return link->mode != SERIAL_OFF;
}

static bool is_serial_port(const char *name) {
    return !strncmp(name, "cu.", 3) || !strncmp(name, "ttyUSB", 6) || !strncmp(name, "ttyACM", 6);
}

static int compare_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int serial_link_ports(char ports[][SERIAL_LINK_PORT_NAME], int max) {
#ifdef __ANDROID__
    (void)ports;
    (void)max;
    return 0;
#endif
    DIR *dev = opendir("/dev");
    if (!dev) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dev)) && count < max) {
        if (!is_serial_port(entry->d_name) || strlen(entry->d_name) + 6 > SERIAL_LINK_PORT_NAME) continue;
        snprintf(ports[count++], SERIAL_LINK_PORT_NAME, "/dev/%s", entry->d_name);
    }
    closedir(dev);
    qsort(ports, (size_t)count, SERIAL_LINK_PORT_NAME, compare_names);
    return count;
}

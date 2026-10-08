#include "app/serial_service.h"

#include <stdio.h>
#include <string.h>

#include "app/host.h"
#include "app/log.h"
#include "app/paths.h"
#include "net/net_gateway.h"

#define RECONNECT_SECONDS 2ull
#define PORT_SCAN_SECONDS 2.0

static void close_link(serial_service_t *service, machine_t *machine) {
    serial_link_close(&service->link);
    machine_serial_connect(machine, false);
}

static bool keeps_link(const serial_service_t *service, serial_mode_t mode) {
    const char *device = service->settings->serial_device;
    if (mode == SERIAL_OFF || mode == SERIAL_NETWORK || service->link.mode != mode) return false;
    return mode != SERIAL_DEVICE || (device && !strcmp(service->link.name, device));
}

static void reconnect_later(serial_service_t *service, machine_t *machine, serial_mode_t mode) {
    service->reconnect_mode = mode;
    service->reconnect_at = machine_cycles(machine) + RECONNECT_SECONDS * MACHINE_CLOCK_HZ;
}

static const char *open_link(serial_service_t *service, machine_t *machine, serial_mode_t mode) {
    serial_link_t *link = &service->link;
    if (keeps_link(service, mode)) {
        machine_serial_connect(machine, false);
    } else {
        close_link(service, machine);
        if (mode == SERIAL_NETWORK) {
            app_rapi_socket_path(service->rapi_socket, sizeof service->rapi_socket);
            link->options.rapi_socket = service->rapi_socket;
        }
        const char *failure = serial_link_open(link, mode, service->settings->serial_device);
        if (failure) return failure;
        if (mode == SERIAL_PTY) fprintf(stderr, "serial: COM1 on %s\n", link->name);
    }
    machine_set_serial_tag(machine, (uint32_t)mode);
    if (mode != SERIAL_OFF && serial_link_attached(link)) machine_serial_connect(machine, true);
    if (mode == SERIAL_NETWORK) return "network cable connected";
    if (mode == SERIAL_TCP) {
        char address[64];
        host_local_address(address, sizeof address);
        snprintf(service->notice, sizeof service->notice, "COM1 at %s:%d", address, link->tcp_port);
        return service->notice;
    }
    if (mode == SERIAL_PTY) return link->name;
    if (mode == SERIAL_DEVICE) {
        snprintf(service->notice, sizeof service->notice, "COM1 on %s", link->name);
        return service->notice;
    }
    return "serial disconnected";
}

void serial_service_init(serial_service_t *service, const settings_t *settings) {
    memset(service, 0, sizeof *service);
    service->settings = settings;
    serial_link_init(&service->link, app_log);
    service->link.options.user_agent = settings->user_agent;
    service->link.options.rapi_port = settings->network_rapi ? (int)settings->rapi_port : 0;
    service->link.tcp_port = (int)settings->serial_tcp_port;
    service->reconnect_mode = SERIAL_OFF;
}

const char *serial_service_start(serial_service_t *service, machine_t *machine, serial_mode_t mode) {
    serial_service_restored(service, machine);
    if (mode == SERIAL_OFF) return NULL;
    if (service->reconnect_at) {
        service->reconnect_mode = mode;
        return NULL;
    }
    return open_link(service, machine, mode);
}

const char *serial_service_select(serial_service_t *service, machine_t *machine, serial_mode_t mode) {
    service->reconnect_at = 0;
    return open_link(service, machine, mode);
}

void serial_service_restored(serial_service_t *service, machine_t *machine) {
    const char *device = service->settings->serial_device;
    bool was_connected = machine_serial_connected(machine);
    serial_mode_t mode = (serial_mode_t)machine_serial_tag(machine);
    machine_serial_connect(machine, false);
    service->reconnect_at = 0;
    bool listening = mode == SERIAL_TCP && service->link.mode == SERIAL_TCP;
    if (listening || (was_connected && (mode == SERIAL_NETWORK || mode == SERIAL_PTY || mode == SERIAL_TCP || (mode == SERIAL_DEVICE && device && device[0])))) {
        reconnect_later(service, machine, mode);
        if (!keeps_link(service, mode)) serial_link_close(&service->link);
    } else {
        serial_link_close(&service->link);
    }
}

void serial_service_replug(serial_service_t *service, machine_t *machine, uint64_t seconds) {
    if (service->link.mode == SERIAL_NETWORK) service->unplug_at = machine_cycles(machine) + seconds * MACHINE_CLOCK_HZ;
}

void serial_service_update_rapi(serial_service_t *service, machine_t *machine) {
    const settings_t *settings = service->settings;
    service->link.options.rapi_port = settings->network_rapi ? (int)settings->rapi_port : 0;
    if (service->link.mode == SERIAL_NETWORK) open_link(service, machine, SERIAL_NETWORK);
}

serial_mode_t serial_service_detach(serial_service_t *service, machine_t *machine) {
    serial_mode_t mode = service->link.mode;
    if (keeps_link(service, mode)) machine_serial_connect(machine, false);
    else close_link(service, machine);
    return mode;
}

void serial_service_attach(serial_service_t *service, machine_t *machine, serial_mode_t mode) {
    service->reconnect_at = service->unplug_at = 0;
    if (mode != SERIAL_OFF) reconnect_later(service, machine, mode);
}

bool serial_service_online(const serial_service_t *service) {
    return service->link.gateway && net_gateway_online(service->link.gateway);
}

void serial_service_step(serial_service_t *service, machine_t *machine, double elapsed, notice_t *notice) {
    serial_link_t *link = &service->link;
    if (service->unplug_at && machine_cycles(machine) >= service->unplug_at) {
        service->unplug_at = 0;
        if (link->mode == SERIAL_NETWORK) {
            open_link(service, machine, SERIAL_OFF);
            reconnect_later(service, machine, SERIAL_NETWORK);
        }
    }
    serial_link_pump(link, machine);
    if (link->mode != SERIAL_TCP) {
        service->tcp_attached = false;
    } else if (serial_link_attached(link) != service->tcp_attached) {
        service->tcp_attached = !service->tcp_attached;
        machine_serial_connect(machine, service->tcp_attached);
        notice_show(notice, service->tcp_attached ? "TCP client connected" : "TCP client disconnected", NOTICE_SHORT);
    }
    if (service->reconnect_at && machine_cycles(machine) >= service->reconnect_at) {
        service->reconnect_at = 0;
        notice_show(notice, open_link(service, machine, service->reconnect_mode), NOTICE_MEDIUM);
    }
    service->since_port_scan -= elapsed;
    if (service->since_port_scan <= 0) {
        service->since_port_scan = PORT_SCAN_SECONDS;
        service->port_count = serial_link_ports(service->ports, SERIAL_PORT_MAX);
    }
}

void serial_service_close(serial_service_t *service, machine_t *machine) {
    close_link(service, machine);
}

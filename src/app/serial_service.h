#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app/notices.h"
#include "app/settings.h"
#include "core/machine.h"
#include "net/serial_link.h"

#define SERIAL_PORT_MAX 16

typedef struct {
    serial_link_t link;
    const settings_t *settings;
    char rapi_socket[1024];
    char notice[SERIAL_LINK_PORT_NAME + 16];
    uint64_t reconnect_at, unplug_at;
    serial_mode_t reconnect_mode;
    bool tcp_attached;
    char ports[SERIAL_PORT_MAX][SERIAL_LINK_PORT_NAME];
    int port_count;
    double since_port_scan;
} serial_service_t;

void          serial_service_init(serial_service_t *service, const settings_t *settings);
const char   *serial_service_start(serial_service_t *service, machine_t *machine, serial_mode_t mode);
const char   *serial_service_select(serial_service_t *service, machine_t *machine, serial_mode_t mode);
void          serial_service_restored(serial_service_t *service, machine_t *machine);
void          serial_service_replug(serial_service_t *service, machine_t *machine, uint64_t seconds);
void          serial_service_update_rapi(serial_service_t *service, machine_t *machine);
serial_mode_t serial_service_detach(serial_service_t *service, machine_t *machine);
void          serial_service_attach(serial_service_t *service, machine_t *machine, serial_mode_t mode);
bool          serial_service_online(const serial_service_t *service);
void          serial_service_step(serial_service_t *service, machine_t *machine, double elapsed, notice_t *notice);
void          serial_service_close(serial_service_t *service, machine_t *machine);

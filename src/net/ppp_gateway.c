#include "net/ppp_gateway.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PROTO_LCP  0xC021
#define PROTO_IPCP 0x8021
#define PROTO_IP   0x0021

#define CONF_REQ 1
#define CONF_ACK 2
#define CONF_NAK 3
#define CONF_REJ 4
#define TERM_REQ 5
#define TERM_ACK 6
#define PROTO_REJ 8
#define ECHO_REQ 9
#define ECHO_REP 10

#define LCP_MRU   1
#define LCP_ACCM  2
#define LCP_MAGIC 5
#define LCP_PFC   7
#define LCP_ACFC  8

#define IPCP_ADDRESS 3
#define IPCP_DNS     129

#define HDLC_FLAG   0x7E
#define HDLC_ESCAPE 0x7D

#define FRAME_MAX   4096
#define OUT_SIZE    (256 * 1024)

#define ETH_HEADER 14
#define ETH_ARP    0x0806
#define ETH_IPV4   0x0800

#define HANDSHAKE        "CLIENT"
#define HANDSHAKE_LENGTH 6

static const uint8_t guest_mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };

struct ppp_gateway {
    net_gateway_t *network;
    net_gateway_log_fn log;
    bool ppp;
    char handshake[64];
    size_t handshake_length;

    uint8_t frame[FRAME_MAX];
    size_t frame_length;
    bool in_frame, escaped;
    uint32_t tx_accm;

    bool lcp_open, lcp_peer_acked, lcp_we_acked;
    uint8_t lcp_request_id;
    uint32_t negotiated_accm;
    bool ipcp_open, ipcp_peer_acked, ipcp_we_acked;
    uint8_t ipcp_request_id;
    uint8_t next_id;

    uint8_t out[OUT_SIZE];
    size_t out_head, out_count;

    bool arp_pending;
    uint8_t arp_mac[6];
    uint8_t arp_ip[4];
};

static void reset_negotiation(ppp_gateway_t *gateway);

static void gateway_log(ppp_gateway_t *gateway, const char *format, ...) {
    if (!gateway->log) return;
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    gateway->log(message);
}

static const uint16_t fcs_table[256] = {
    0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf, 0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
    0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e, 0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
    0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd, 0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
    0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c, 0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
    0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb, 0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
    0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a, 0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
    0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9, 0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
    0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738, 0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
    0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7, 0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
    0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036, 0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
    0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5, 0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
    0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134, 0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
    0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3, 0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
    0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232, 0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
    0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1, 0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
    0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330, 0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78,
};

static uint16_t fcs16(uint16_t fcs, const uint8_t *data, size_t length) {
    while (length--) fcs = (uint16_t)((fcs >> 8) ^ fcs_table[(fcs ^ *data++) & 0xFF]);
    return fcs;
}

static void out_byte(ppp_gateway_t *gateway, uint8_t byte) {
    if (gateway->out_count == OUT_SIZE) return;
    gateway->out[(gateway->out_head + gateway->out_count) % OUT_SIZE] = byte;
    gateway->out_count++;
}

static void out_escaped(ppp_gateway_t *gateway, uint8_t byte) {
    if (byte == HDLC_FLAG || byte == HDLC_ESCAPE || (byte < 0x20 && (gateway->tx_accm & (1u << byte)))) {
        out_byte(gateway, HDLC_ESCAPE);
        out_byte(gateway, byte ^ 0x20);
    } else {
        out_byte(gateway, byte);
    }
}

static void send_ppp(ppp_gateway_t *gateway, uint16_t protocol, const uint8_t *payload, size_t length) {
    uint8_t header[4] = { 0xFF, 0x03, (uint8_t)(protocol >> 8), (uint8_t)protocol };
    uint16_t fcs = fcs16(0xFFFF, header, 4);
    fcs = fcs16(fcs, payload, length) ^ 0xFFFF;
    out_byte(gateway, HDLC_FLAG);
    for (int i = 0; i < 4; i++) out_escaped(gateway, header[i]);
    for (size_t i = 0; i < length; i++) out_escaped(gateway, payload[i]);
    out_escaped(gateway, (uint8_t)fcs);
    out_escaped(gateway, (uint8_t)(fcs >> 8));
    out_byte(gateway, HDLC_FLAG);
}

static void send_control(ppp_gateway_t *gateway, uint16_t protocol, uint8_t code, uint8_t id, const uint8_t *data, size_t length) {
    uint8_t packet[FRAME_MAX];
    if (length + 4 > sizeof packet) return;
    packet[0] = code;
    packet[1] = id;
    packet[2] = (uint8_t)((length + 4) >> 8);
    packet[3] = (uint8_t)(length + 4);
    if (length) memcpy(packet + 4, data, length);
    send_ppp(gateway, protocol, packet, length + 4);
}

static void send_lcp_request(ppp_gateway_t *gateway) {
    gateway->lcp_request_id = gateway->next_id++;
    send_control(gateway, PROTO_LCP, CONF_REQ, gateway->lcp_request_id, NULL, 0);
}

static void send_ipcp_request(ppp_gateway_t *gateway) {
    uint8_t option[6] = { IPCP_ADDRESS, 6, NET_GATEWAY_HOST_IP[0], NET_GATEWAY_HOST_IP[1], NET_GATEWAY_HOST_IP[2], NET_GATEWAY_HOST_IP[3] };
    gateway->ipcp_request_id = gateway->next_id++;
    send_control(gateway, PROTO_IPCP, CONF_REQ, gateway->ipcp_request_id, option, sizeof option);
}

static void put16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value >> 8); p[1] = (uint8_t)value;
}

static void send_arp(ppp_gateway_t *gateway, const uint8_t *destination_mac, uint16_t operation,
                     const uint8_t *target_mac, const uint8_t *target_ip) {
    uint8_t frame[ETH_HEADER + 28];
    memcpy(frame, destination_mac, 6);
    memcpy(frame + 6, guest_mac, 6);
    put16(frame + 12, ETH_ARP);
    uint8_t *arp = frame + ETH_HEADER;
    put16(arp, 1);
    put16(arp + 2, ETH_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    put16(arp + 6, operation);
    memcpy(arp + 8, guest_mac, 6);
    memcpy(arp + 14, NET_GATEWAY_GUEST_IP, 4);
    memcpy(arp + 18, target_mac, 6);
    memcpy(arp + 24, target_ip, 4);
    net_gateway_input(gateway->network, frame, sizeof frame);
}

static void maybe_open_lcp(ppp_gateway_t *gateway) {
    if (gateway->lcp_open || !gateway->lcp_peer_acked || !gateway->lcp_we_acked) return;
    gateway->lcp_open = true;
    gateway->tx_accm = gateway->negotiated_accm;
    gateway_log(gateway, "ppp: LCP up\n");
}

static void maybe_open_ipcp(ppp_gateway_t *gateway) {
    if (gateway->ipcp_open || !gateway->ipcp_peer_acked || !gateway->ipcp_we_acked) return;
    gateway->ipcp_open = true;
    gateway_log(gateway, "ppp: IPCP up, guest 10.0.2.15 online\n");
    static const uint8_t no_mac[6] = { 0 };
    send_arp(gateway, NET_GATEWAY_HOST_MAC, 1, no_mac, NET_GATEWAY_GUEST_IP);
}

static void restart_ipcp(ppp_gateway_t *gateway) {
    gateway->ipcp_open = gateway->ipcp_peer_acked = gateway->ipcp_we_acked = false;
    gateway->ipcp_request_id = 0;
}

static void restart_lcp(ppp_gateway_t *gateway) {
    gateway->lcp_open = gateway->lcp_peer_acked = gateway->lcp_we_acked = false;
    gateway->lcp_request_id = 0;
    gateway->tx_accm = gateway->negotiated_accm = 0xFFFFFFFFu;
    restart_ipcp(gateway);
}

static void lcp_request(ppp_gateway_t *gateway, uint8_t id, const uint8_t *options, size_t length) {
    uint8_t reject[FRAME_MAX];
    size_t rejected = 0;
    for (size_t i = 0; i + 2 <= length;) {
        uint8_t type = options[i], option_length = options[i + 1];
        if (option_length < 2 || i + option_length > length) break;
        const uint8_t *value = options + i + 2;
        bool refuse = false;
        switch (type) {
        case LCP_MRU: case LCP_PFC: case LCP_ACFC: break;
        case LCP_ACCM:
            if (option_length == 6) gateway->negotiated_accm = (uint32_t)value[0] << 24 | value[1] << 16 | value[2] << 8 | value[3];
            break;
        case LCP_MAGIC:
            refuse = option_length == 6 && !value[0] && !value[1] && !value[2] && !value[3];
            break;
        default: refuse = true; break;
        }
        if (refuse) {
            memcpy(reject + rejected, options + i, option_length);
            rejected += option_length;
        }
        i += option_length;
    }
    if (rejected) {
        send_control(gateway, PROTO_LCP, CONF_REJ, id, reject, rejected);
    } else {
        send_control(gateway, PROTO_LCP, CONF_ACK, id, options, length);
        gateway->lcp_we_acked = true;
        maybe_open_lcp(gateway);
    }
}

static void ipcp_request(ppp_gateway_t *gateway, uint8_t id, const uint8_t *options, size_t length) {
    uint8_t reject[FRAME_MAX], nak[FRAME_MAX];
    size_t rejected = 0, naked = 0;
    for (size_t i = 0; i + 2 <= length;) {
        uint8_t type = options[i], option_length = options[i + 1];
        if (option_length < 2 || i + option_length > length) break;
        const uint8_t *value = options + i + 2;
        const uint8_t *wanted = type == IPCP_ADDRESS ? NET_GATEWAY_GUEST_IP : type == IPCP_DNS ? NET_GATEWAY_DNS_IP : NULL;
        if (wanted && option_length == 6) {
            if (memcmp(value, wanted, 4) != 0) {
                nak[naked++] = type;
                nak[naked++] = 6;
                memcpy(nak + naked, wanted, 4);
                naked += 4;
            }
        } else {
            memcpy(reject + rejected, options + i, option_length);
            rejected += option_length;
        }
        i += option_length;
    }
    if (rejected) send_control(gateway, PROTO_IPCP, CONF_REJ, id, reject, rejected);
    else if (naked) send_control(gateway, PROTO_IPCP, CONF_NAK, id, nak, naked);
    else {
        send_control(gateway, PROTO_IPCP, CONF_ACK, id, options, length);
        gateway->ipcp_we_acked = true;
        maybe_open_ipcp(gateway);
    }
}

static void control_packet(ppp_gateway_t *gateway, uint16_t protocol, const uint8_t *packet, size_t length) {
    if (length < 4) return;
    uint8_t code = packet[0], id = packet[1];
    size_t packet_length = (size_t)packet[2] << 8 | packet[3];
    if (packet_length < 4 || packet_length > length) packet_length = length;
    const uint8_t *data = packet + 4;
    size_t data_length = packet_length - 4;
    bool lcp = protocol == PROTO_LCP;
    switch (code) {
    case CONF_REQ:
        if (lcp && gateway->lcp_open) {
            restart_lcp(gateway);
            gateway_log(gateway, "ppp: LCP restarted by guest\n");
        } else if (!lcp && gateway->ipcp_open) {
            restart_ipcp(gateway);
            gateway_log(gateway, "ppp: IPCP restarted by guest\n");
        }
        if (lcp) {
            lcp_request(gateway, id, data, data_length);
            if (!gateway->lcp_request_id) send_lcp_request(gateway);
        } else {
            ipcp_request(gateway, id, data, data_length);
            if (!gateway->ipcp_request_id) send_ipcp_request(gateway);
        }
        break;
    case CONF_ACK:
        if (lcp && id == gateway->lcp_request_id) { gateway->lcp_peer_acked = true; maybe_open_lcp(gateway); }
        if (!lcp && id == gateway->ipcp_request_id) { gateway->ipcp_peer_acked = true; maybe_open_ipcp(gateway); }
        break;
    case CONF_NAK:
    case CONF_REJ:
        if (lcp) send_lcp_request(gateway);
        else send_ipcp_request(gateway);
        break;
    case TERM_REQ:
        send_control(gateway, protocol, TERM_ACK, id, data, data_length);
        if (lcp) reset_negotiation(gateway);
        else gateway->ipcp_open = false;
        gateway_log(gateway, "ppp: %s terminated by guest\n", lcp ? "LCP" : "IPCP");
        break;
    case ECHO_REQ:
        if (lcp && gateway->lcp_open) {
            uint8_t reply[FRAME_MAX] = { 0 };
            size_t reply_length = 4;
            if (data_length > 4 && data_length - 4 + 4 <= sizeof reply) {
                memcpy(reply + 4, data + 4, data_length - 4);
                reply_length = data_length;
            }
            send_control(gateway, PROTO_LCP, ECHO_REP, id, reply, reply_length);
        }
        break;
    default:
        break;
    }
}

static void ppp_frame(ppp_gateway_t *gateway, const uint8_t *frame, size_t length) {
    if (length < 3 || fcs16(0xFFFF, frame, length) != 0xF0B8) return;
    length -= 2;
    if (length >= 2 && frame[0] == 0xFF && frame[1] == 0x03) { frame += 2; length -= 2; }
    if (length < 1) return;
    uint16_t protocol;
    if (frame[0] & 1) { protocol = frame[0]; frame++; length--; }
    else {
        if (length < 2) return;
        protocol = (uint16_t)(frame[0] << 8 | frame[1]);
        frame += 2;
        length -= 2;
    }
    if (protocol == PROTO_LCP || protocol == PROTO_IPCP) {
        control_packet(gateway, protocol, frame, length);
    } else if (protocol == PROTO_IP) {
        if (!gateway->ipcp_open || length == 0 || length + ETH_HEADER > FRAME_MAX) return;
        uint8_t ethernet[FRAME_MAX];
        memcpy(ethernet, NET_GATEWAY_HOST_MAC, 6);
        memcpy(ethernet + 6, guest_mac, 6);
        put16(ethernet + 12, ETH_IPV4);
        memcpy(ethernet + ETH_HEADER, frame, length);
        net_gateway_input(gateway->network, ethernet, length + ETH_HEADER);
    } else if (gateway->lcp_open) {
        uint8_t reject[FRAME_MAX];
        if (length + 2 > sizeof reject) length = sizeof reject - 2;
        reject[0] = (uint8_t)(protocol >> 8);
        reject[1] = (uint8_t)protocol;
        memcpy(reject + 2, frame, length);
        send_control(gateway, PROTO_LCP, PROTO_REJ, gateway->next_id++, reject, length + 2);
    }
}

static void from_network(void *context, const uint8_t *ethernet, size_t length) {
    ppp_gateway_t *gateway = context;
    if (length < ETH_HEADER) return;
    uint16_t type = (uint16_t)(ethernet[12] << 8 | ethernet[13]);
    if (type == ETH_ARP && length >= ETH_HEADER + 28) {
        const uint8_t *arp = ethernet + ETH_HEADER;
        if (((arp[6] << 8) | arp[7]) == 1 && memcmp(arp + 24, NET_GATEWAY_GUEST_IP, 4) == 0) {
            memcpy(gateway->arp_mac, arp + 8, 6);
            memcpy(gateway->arp_ip, arp + 14, 4);
            gateway->arp_pending = true;
        }
    } else if (type == ETH_IPV4 && gateway->ipcp_open) {
        send_ppp(gateway, PROTO_IP, ethernet + ETH_HEADER, length - ETH_HEADER);
    }
}

ppp_gateway_t *ppp_gateway_create(net_gateway_log_fn log, const net_gateway_options_t *options) {
    ppp_gateway_t *gateway = calloc(1, sizeof *gateway);
    if (!gateway) return NULL;
    gateway->log = log;
    gateway->network = net_gateway_create(log, options, from_network, gateway);
    if (!gateway->network) {
        free(gateway);
        return NULL;
    }
    ppp_gateway_reset(gateway);
    return gateway;
}

void ppp_gateway_destroy(ppp_gateway_t *gateway) {
    if (!gateway) return;
    net_gateway_destroy(gateway->network);
    free(gateway);
}

static void reset_negotiation(ppp_gateway_t *gateway) {
    gateway->ppp = false;
    gateway->handshake_length = 0;
    gateway->frame_length = 0;
    gateway->in_frame = gateway->escaped = false;
    gateway->tx_accm = 0xFFFFFFFFu;
    gateway->negotiated_accm = 0xFFFFFFFFu;
    gateway->lcp_open = gateway->lcp_peer_acked = gateway->lcp_we_acked = false;
    gateway->ipcp_open = gateway->ipcp_peer_acked = gateway->ipcp_we_acked = false;
    gateway->lcp_request_id = gateway->ipcp_request_id = 0;
    gateway->next_id = 1;
}

void ppp_gateway_reset(ppp_gateway_t *gateway) {
    reset_negotiation(gateway);
    gateway->out_head = gateway->out_count = 0;
}

static void handshake_reply(ppp_gateway_t *gateway) {
    static const char reply[] = "CLIENTSERVER";
    for (size_t i = 0; i < sizeof reply - 1; i++) out_byte(gateway, (uint8_t)reply[i]);
    gateway_log(gateway, "ppp: direct connection handshake\n");
    gateway->handshake_length = 0;
    gateway->ppp = true;
}

static void handshake_byte(ppp_gateway_t *gateway, uint8_t byte) {
    if (byte == HDLC_FLAG) {
        gateway->ppp = true;
        return;
    }
    if (gateway->handshake_length < sizeof gateway->handshake - 1) {
        gateway->handshake[gateway->handshake_length++] = (char)byte;
        gateway->handshake[gateway->handshake_length] = 0;
    }
    if (strstr(gateway->handshake, HANDSHAKE)) handshake_reply(gateway);
}

void ppp_gateway_from_guest(ppp_gateway_t *gateway, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
        uint8_t byte = data[i];
        if (!gateway->ppp) {
            handshake_byte(gateway, byte);
            if (!gateway->ppp || byte != HDLC_FLAG) continue;
        }
        if (byte == HDLC_FLAG) {
            bool complete = gateway->in_frame && !gateway->escaped && gateway->frame_length;
            size_t frame_length = gateway->frame_length;
            gateway->frame_length = 0;
            gateway->in_frame = true;
            gateway->escaped = false;
            if (complete) ppp_frame(gateway, gateway->frame, frame_length);
            continue;
        }
        if (!gateway->in_frame) {
            if (!gateway->lcp_open) handshake_byte(gateway, byte);
            continue;
        }
        if (byte == HDLC_ESCAPE) { gateway->escaped = true; continue; }
        if (gateway->escaped) { byte ^= 0x20; gateway->escaped = false; }
        if (gateway->frame_length < FRAME_MAX) gateway->frame[gateway->frame_length++] = byte;
        if (gateway->frame_length == HANDSHAKE_LENGTH && !memcmp(gateway->frame, HANDSHAKE, HANDSHAKE_LENGTH)) {
            reset_negotiation(gateway);
            handshake_reply(gateway);
        }
    }
}

size_t ppp_gateway_to_guest(ppp_gateway_t *gateway, uint8_t *out, size_t max) {
    size_t count = gateway->out_count < max ? gateway->out_count : max;
    for (size_t i = 0; i < count; i++) out[i] = gateway->out[(gateway->out_head + i) % OUT_SIZE];
    gateway->out_head = (gateway->out_head + count) % OUT_SIZE;
    gateway->out_count -= count;
    return count;
}

void ppp_gateway_poll(ppp_gateway_t *gateway, uint64_t guest_ms) {
    if (gateway->arp_pending) {
        gateway->arp_pending = false;
        send_arp(gateway, gateway->arp_mac, 2, gateway->arp_mac, gateway->arp_ip);
    }
    net_gateway_poll(gateway->network, guest_ms);
}

bool ppp_gateway_online(const ppp_gateway_t *gateway) {
    return gateway->ipcp_open;
}

net_gateway_t *ppp_gateway_network(ppp_gateway_t *gateway) {
    return gateway->network;
}

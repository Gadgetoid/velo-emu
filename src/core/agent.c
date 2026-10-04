#include "core/agent.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define FRAME_HEADER 4
#define INPUT_MAX    (FRAME_HEADER + MAILBOX_MESSAGE_MAX)

struct agent {
    int          listener;
    int          client;
    char         path[sizeof ((struct sockaddr_un *)0)->sun_path];
    agent_log_fn log;
    uint8_t      input[INPUT_MAX];
    size_t       input_length;
};

static void agent_log(agent_t *agent, const char *message) {
    if (agent->log) agent->log(message);
}

agent_t *agent_create(const char *socket_path, agent_log_fn log) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof address.sun_path) return NULL;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0) return NULL;
    unlink(socket_path);
    if (bind(listener, (struct sockaddr *)&address, sizeof address) < 0 || listen(listener, 1) < 0) {
        close(listener);
        return NULL;
    }
    agent_t *agent = calloc(1, sizeof *agent);
    agent->listener = listener;
    agent->client = -1;
    agent->log = log;
    snprintf(agent->path, sizeof agent->path, "%s", socket_path);
    return agent;
}

static void disconnect(agent_t *agent, mailbox_t *mailbox) {
    if (agent->client >= 0) {
        close(agent->client);
        agent_log(agent, "agent: host disconnected\n");
    }
    agent->client = -1;
    agent->input_length = 0;
    if (!mailbox) return;
    mailbox->connected = false;
    mailbox_clear_host(mailbox);
}

void agent_destroy(agent_t *agent) {
    if (!agent) return;
    disconnect(agent, NULL);
    close(agent->listener);
    unlink(agent->path);
    free(agent);
}

static bool send_all(int fd, const uint8_t *data, size_t length) {
    while (length) {
        ssize_t sent = send(fd, data, length, SEND_FLAGS);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static void receive_frames(agent_t *agent, mailbox_t *mailbox);

static void accept_host(agent_t *agent, mailbox_t *mailbox) {
    struct pollfd poll_fd = { .fd = agent->listener, .events = POLLIN };
    if (poll(&poll_fd, 1, 0) <= 0) return;
    if (agent->client >= 0) receive_frames(agent, mailbox);
    int client = accept(agent->listener, NULL, NULL);
    if (client < 0) return;
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    if (agent->client >= 0) {
        close(client);
        return;
    }
    agent->client = client;
    agent->input_length = 0;
    mailbox_clear_host(mailbox);
    mailbox->connected = true;
    agent_log(agent, "agent: host connected\n");
}

static bool parse_frames(agent_t *agent, mailbox_t *mailbox) {
    while (agent->input_length >= FRAME_HEADER && mailbox->to_guest.count < MAILBOX_QUEUE) {
        uint32_t length = (uint32_t)agent->input[0] | (uint32_t)agent->input[1] << 8 | (uint32_t)agent->input[2] << 16 | (uint32_t)agent->input[3] << 24;
        if (length > MAILBOX_MESSAGE_MAX) {
            agent_log(agent, "agent: frame too large, disconnecting\n");
            disconnect(agent, mailbox);
            return false;
        }
        if (agent->input_length < FRAME_HEADER + length) break;
        mailbox_push(&mailbox->to_guest, agent->input + FRAME_HEADER, length);
        size_t used = FRAME_HEADER + length;
        memmove(agent->input, agent->input + used, agent->input_length - used);
        agent->input_length -= used;
    }
    return true;
}

static void receive_frames(agent_t *agent, mailbox_t *mailbox) {
    struct pollfd poll_fd = { .fd = agent->client, .events = POLLIN };
    while (parse_frames(agent, mailbox) && mailbox->to_guest.count < MAILBOX_QUEUE && agent->input_length < sizeof agent->input &&
           poll(&poll_fd, 1, 0) > 0) {
        ssize_t received = recv(agent->client, agent->input + agent->input_length, sizeof agent->input - agent->input_length, 0);
        if (received <= 0) {
            disconnect(agent, mailbox);
            return;
        }
        agent->input_length += (size_t)received;
    }
}

static void send_frames(agent_t *agent, mailbox_t *mailbox) {
    const mailbox_message_t *message;
    while (agent->client >= 0 && (message = mailbox_peek(&mailbox->to_host))) {
        uint8_t header[FRAME_HEADER] = { (uint8_t)message->length, (uint8_t)(message->length >> 8), (uint8_t)(message->length >> 16),
                                         (uint8_t)(message->length >> 24) };
        if (!send_all(agent->client, header, sizeof header) || !send_all(agent->client, message->data, message->length)) {
            disconnect(agent, mailbox);
            return;
        }
        mailbox_pop(&mailbox->to_host);
    }
}

void agent_poll(agent_t *agent, mailbox_t *mailbox) {
    accept_host(agent, mailbox);
    mailbox->connected = agent->client >= 0;
    if (agent->client < 0) return;
    receive_frames(agent, mailbox);
    send_frames(agent, mailbox);
}

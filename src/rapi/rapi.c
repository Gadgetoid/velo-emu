#include "rapi/rapi.h"
#include "rapi/rapi_project.h"

#include <netdb.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define COMMAND_FIND_ALL_FILES     0x09
#define COMMAND_CREATE_FILE        0x05
#define COMMAND_READ_FILE          0x06
#define COMMAND_WRITE_FILE         0x07
#define COMMAND_CLOSE_HANDLE       0x08
#define COMMAND_CREATE_DIRECTORY   0x17
#define COMMAND_REMOVE_DIRECTORY   0x18
#define COMMAND_CREATE_PROCESS     0x19
#define COMMAND_MOVE_FILE          0x1A
#define COMMAND_DELETE_FILE        0x1C
#define COMMAND_REG_OPEN_KEY       0x1E
#define COMMAND_REG_ENUM_KEY       0x1F
#define COMMAND_REG_CREATE_KEY     0x20
#define COMMAND_REG_CLOSE_KEY      0x21
#define COMMAND_REG_ENUM_VALUE     0x23
#define COMMAND_REG_QUERY_VALUE    0x26
#define COMMAND_REG_SET_VALUE      0x27
#define COMMAND_GET_STORE_INFO     0x29
#define COMMAND_GET_VERSION        0x3B

#define FIND_ATTRIBUTES      0x01
#define FIND_LAST_WRITE_TIME 0x08
#define FIND_SIZE_LOW        0x20
#define FIND_NAME            0x80

#define GENERIC_READ   0x80000000u
#define GENERIC_WRITE  0x40000000u
#define CREATE_ALWAYS  2
#define OPEN_EXISTING  3
#define ATTRIBUTE_NORMAL 0x80
#define INVALID_HANDLE 0xFFFFFFFFu
#define ERROR_NO_MORE_ITEMS 259

#define CHUNK_SIZE      8192
#define REPLY_MAX       (4 * 1024 * 1024)
#define HANDSHAKE_TIMEOUT 5
#define REPLY_TIMEOUT     30
#define FIND_ENTRY_MIN  20

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

struct rapi {
    int socket;
    uint32_t os_major;
    uint8_t *reply;
    size_t reply_length, reply_offset;
    char error[512];
};

typedef struct {
    uint8_t *data;
    size_t length, capacity;
} message_t;

static void set_error(rapi_t *rapi, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(rapi->error, sizeof rapi->error, format, args);
    va_end(args);
}

static const char *error_name(uint32_t code) {
    switch (code) {
    case 2: return "file not found";
    case 3: return "path not found";
    case 5: return "access denied";
    case 32: return "file in use";
    case 80: return "file exists";
    case 112: return "not enough storage";
    case 123: return "invalid name";
    case 145: return "folder not empty";
    case 183: return "already exists";
    }
    return "error";
}

static bool ce_failed(rapi_t *rapi, const char *what, const char *path, uint32_t code) {
    set_error(rapi, "%s %s: %s (%u)", what, path, error_name(code), code);
    return false;
}

static bool message_bytes(message_t *message, const void *data, size_t length) {
    if (message->length + length > message->capacity) {
        size_t capacity = message->capacity ? message->capacity : 256;
        while (capacity < message->length + length) capacity *= 2;
        uint8_t *grown = realloc(message->data, capacity);
        if (!grown) return false;
        message->data = grown;
        message->capacity = capacity;
    }
    memcpy(message->data + message->length, data, length);
    message->length += length;
    return true;
}

static bool message_u32(message_t *message, uint32_t value) {
    uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };
    return message_bytes(message, bytes, 4);
}

static size_t utf8_to_utf16(const char *text, uint16_t *out, size_t max) {
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && count + 2 < max) {
        uint32_t codepoint;
        if (*p < 0x80) codepoint = *p++;
        else if ((*p & 0xE0) == 0xC0 && p[1]) { codepoint = (uint32_t)(*p & 0x1F) << 6 | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { codepoint = (uint32_t)(*p & 0x0F) << 12 | (uint32_t)(p[1] & 0x3F) << 6 | (p[2] & 0x3F); p += 3; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
            codepoint = (uint32_t)(*p & 0x07) << 18 | (uint32_t)(p[1] & 0x3F) << 12 | (uint32_t)(p[2] & 0x3F) << 6 | (p[3] & 0x3F);
            p += 4;
        }
        else { codepoint = '?'; p++; }
        if (codepoint >= 0x10000) {
            codepoint -= 0x10000;
            out[count++] = (uint16_t)(0xD800 | (codepoint >> 10));
            out[count++] = (uint16_t)(0xDC00 | (codepoint & 0x3FF));
        } else {
            out[count++] = (uint16_t)codepoint;
        }
    }
    out[count++] = 0;
    return count;
}

static void utf16_to_utf8(const uint8_t *data, size_t units, char *out, size_t max) {
    size_t length = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t codepoint = (uint32_t)data[i * 2] | (uint32_t)data[i * 2 + 1] << 8;
        if (codepoint == 0) break;
        if (codepoint >= 0xD800 && codepoint < 0xDC00 && i + 1 < units) {
            uint32_t low = (uint32_t)data[i * 2 + 2] | (uint32_t)data[i * 2 + 3] << 8;
            if (low >= 0xDC00 && low < 0xE000) {
                codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                i++;
            }
        }
        char encoded[4];
        size_t size;
        if (codepoint < 0x80) { encoded[0] = (char)codepoint; size = 1; }
        else if (codepoint < 0x800) { encoded[0] = (char)(0xC0 | codepoint >> 6); encoded[1] = (char)(0x80 | (codepoint & 0x3F)); size = 2; }
        else if (codepoint < 0x10000) {
            encoded[0] = (char)(0xE0 | codepoint >> 12); encoded[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
            encoded[2] = (char)(0x80 | (codepoint & 0x3F)); size = 3;
        } else {
            encoded[0] = (char)(0xF0 | codepoint >> 18); encoded[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
            encoded[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F)); encoded[3] = (char)(0x80 | (codepoint & 0x3F)); size = 4;
        }
        if (length + size >= max) break;
        memcpy(out + length, encoded, size);
        length += size;
    }
    out[length] = 0;
}

static size_t encode_utf16(const char *text, uint8_t *bytes) {
    uint16_t wide[RAPI_NAME_MAX];
    size_t units = utf8_to_utf16(text, wide, RAPI_NAME_MAX);
    for (size_t i = 0; i < units; i++) {
        bytes[i * 2] = (uint8_t)wide[i];
        bytes[i * 2 + 1] = (uint8_t)(wide[i] >> 8);
    }
    return units;
}

static bool message_string(message_t *message, const char *text) {
    uint8_t bytes[RAPI_NAME_MAX * 2];
    size_t units = encode_utf16(text, bytes);
    return message_u32(message, 1) && message_u32(message, (uint32_t)units) && message_bytes(message, bytes, units * 2);
}

static bool message_optional_string(message_t *message, const char *text) {
    if (!text) return message_u32(message, 0);
    uint8_t bytes[RAPI_NAME_MAX * 2];
    size_t units = encode_utf16(text, bytes);
    return message_u32(message, 1) && message_u32(message, (uint32_t)(units * 2)) && message_u32(message, 1) &&
           message_bytes(message, bytes, units * 2);
}

static bool message_optional_out(message_t *message, uint32_t size) {
    return message_u32(message, 1) && message_u32(message, size) && message_u32(message, 0);
}

static bool message_begin(message_t *message, uint32_t command) {
    message->length = 0;
    return message_u32(message, command);
}

static bool socket_read(int socket, void *data, size_t length) {
    uint8_t *p = data;
    while (length) {
        ssize_t got = recv(socket, p, length, 0);
        if (got <= 0) return false;
        p += got;
        length -= (size_t)got;
    }
    return true;
}

static bool socket_write(int socket, const void *data, size_t length) {
    const uint8_t *p = data;
    while (length) {
        ssize_t sent = send(socket, p, length, SEND_FLAGS);
        if (sent <= 0) return false;
        p += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool reply_u32(rapi_t *rapi, uint32_t *value) {
    if (rapi->reply_offset + 4 > rapi->reply_length) {
        set_error(rapi, "short reply from the " RAPI_DEVICE);
        return false;
    }
    const uint8_t *p = rapi->reply + rapi->reply_offset;
    *value = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    rapi->reply_offset += 4;
    return true;
}

static const uint8_t *reply_bytes(rapi_t *rapi, size_t length) {
    if (rapi->reply_offset + length > rapi->reply_length) {
        set_error(rapi, "short reply from the " RAPI_DEVICE);
        return NULL;
    }
    const uint8_t *p = rapi->reply + rapi->reply_offset;
    rapi->reply_offset += length;
    return p;
}

static bool connection_broken(rapi_t *rapi, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(rapi->error, sizeof rapi->error, format, args);
    va_end(args);
    close(rapi->socket);
    rapi->socket = -1;
    rapi->reply_length = 0;
    rapi->reply_offset = 0;
    return false;
}

static bool call(rapi_t *rapi, message_t *message) {
    uint8_t size[4] = { (uint8_t)message->length, (uint8_t)(message->length >> 8), (uint8_t)(message->length >> 16), (uint8_t)(message->length >> 24) };
    bool connected = rapi->socket >= 0;
    bool sent = connected && socket_write(rapi->socket, size, 4) && socket_write(rapi->socket, message->data, message->length);
    free(message->data);
    *message = (message_t){ 0 };
    if (!connected) return false;
    if (!sent) return connection_broken(rapi, "lost the connection to the " RAPI_DEVICE);
    uint8_t header[4];
    if (!socket_read(rapi->socket, header, 4)) return connection_broken(rapi, "the " RAPI_DEVICE " closed the connection");
    size_t length = (size_t)header[0] | (size_t)header[1] << 8 | (size_t)header[2] << 16 | (size_t)header[3] << 24;
    if (length > REPLY_MAX) return connection_broken(rapi, "reply too large (%zu bytes)", length);
    uint8_t *reply = realloc(rapi->reply, length ? length : 1);
    if (!reply) return connection_broken(rapi, "out of memory for a %zu byte reply", length);
    rapi->reply = reply;
    rapi->reply_length = length;
    rapi->reply_offset = 0;
    if (!socket_read(rapi->socket, rapi->reply, length)) return connection_broken(rapi, "the " RAPI_DEVICE " closed the connection");
    uint32_t status;
    if (!reply_u32(rapi, &status)) return false;
    if (status == 1) {
        uint32_t code = 0;
        reply_u32(rapi, &code);
        set_error(rapi, "the " RAPI_DEVICE " rejected the call (0x%08x)", code);
        return false;
    }
    return true;
}

static bool call_result(rapi_t *rapi, message_t *message, uint32_t *last_error, uint32_t *result) {
    return call(rapi, message) && reply_u32(rapi, last_error) && reply_u32(rapi, result);
}

bool rapi_data_path(const char *leaf, char *path, size_t size) {
    const char *data_home = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME") ? getenv("HOME") : ".";
    int length;
    if (data_home && data_home[0] == '/') length = snprintf(path, size, "%s/" RAPI_DATA_FOLDER "/%s", data_home, leaf);
#ifdef __APPLE__
    else length = snprintf(path, size, "%s/Library/Application Support/" RAPI_MAC_FOLDER "/%s", home, leaf);
#else
    else length = snprintf(path, size, "%s/.local/share/" RAPI_DATA_FOLDER "/%s", home, leaf);
#endif
    return length > 0 && (size_t)length < size;
}

static rapi_t *rapi_start(int fd, char *error, size_t error_size);

rapi_t *rapi_connect(const char *socket_path, char *error, size_t error_size) {
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
        if (fd >= 0) close(fd);
        snprintf(error, error_size, "the emulator isn't running with Network (PPP) connected (no %s)", socket_path);
        return NULL;
    }
    return rapi_start(fd, error, error_size);
}

rapi_t *rapi_connect_tcp(const char *host, const char *port, char *error, size_t error_size) {
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *found = NULL;
    if (getaddrinfo(host, port, &hints, &found) != 0 || !found) {
        snprintf(error, error_size, "cannot find %s", host);
        return NULL;
    }
    int fd = -1;
    for (struct addrinfo *at = found; at && fd < 0; at = at->ai_next) {
        fd = socket(at->ai_family, at->ai_socktype, at->ai_protocol);
        if (fd >= 0 && connect(fd, at->ai_addr, at->ai_addrlen) != 0) {
            close(fd);
            fd = -1;
        }
    }
    freeaddrinfo(found);
    if (fd < 0) {
        snprintf(error, error_size, "cannot connect to %s:%s (is RAPI over the Network on, with Network (PPP) connected?)", host, port);
        return NULL;
    }
    return rapi_start(fd, error, error_size);
}

static rapi_t *rapi_start(int fd, char *error, size_t error_size) {
    struct timeval timeout = { HANDSHAKE_TIMEOUT, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
    rapi_t *rapi = calloc(1, sizeof *rapi);
    rapi->socket = fd;
    rapi_version_t version;
    if (!rapi_version(rapi, &version)) {
        snprintf(error, error_size, "the " RAPI_DEVICE " isn't answering (is PC Link connected?): %s", rapi->error);
        rapi_disconnect(rapi);
        return NULL;
    }
    rapi->os_major = version.major;
    rapi_set_timeout(rapi, REPLY_TIMEOUT);
    return rapi;
}

uint32_t rapi_os_major(const rapi_t *rapi) {
    return rapi->os_major;
}

void rapi_set_timeout(rapi_t *rapi, int seconds) {
    if (rapi->socket < 0 || seconds <= 0) return;
    struct timeval timeout = { seconds, 0 };
    setsockopt(rapi->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
}

void rapi_disconnect(rapi_t *rapi) {
    if (!rapi) return;
    if (rapi->socket >= 0) close(rapi->socket);
    free(rapi->reply);
    free(rapi);
}

const char *rapi_error(const rapi_t *rapi) {
    return rapi->error;
}

bool rapi_version(rapi_t *rapi, rapi_version_t *version) {
    message_t message = { 0 };
    uint32_t last_error, result, size;
    if (!message_begin(&message, COMMAND_GET_VERSION) || !call_result(rapi, &message, &last_error, &result) || !reply_u32(rapi, &size)) return false;
    const uint8_t *data = reply_bytes(rapi, size);
    if (!data || size < 20) return false;
    uint32_t fields[5];
    for (int i = 0; i < 5; i++) fields[i] = (uint32_t)data[i * 4] | (uint32_t)data[i * 4 + 1] << 8 | (uint32_t)data[i * 4 + 2] << 16 | (uint32_t)data[i * 4 + 3] << 24;
    *version = (rapi_version_t){ fields[1], fields[2], fields[3], fields[4] };
    return true;
}

bool rapi_store(rapi_t *rapi, rapi_store_t *store) {
    message_t message = { 0 };
    uint32_t last_error, result, present, size, has_value;
    if (!message_begin(&message, COMMAND_GET_STORE_INFO) || !message_optional_out(&message, 8)) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    if (!result) return ce_failed(rapi, "store information", "", last_error);
    if (!reply_u32(rapi, &present) || !present || !reply_u32(rapi, &size) || !reply_u32(rapi, &has_value) || size < 8) return false;
    return reply_u32(rapi, &store->store_size) && reply_u32(rapi, &store->free_size);
}

static bool safe_file_name(const uint8_t *units, uint32_t unit_count, const char *name) {
    bool ended = false;
    for (uint32_t i = 0; i < unit_count; i++) {
        bool terminator = !units[i * 2] && !units[i * 2 + 1];
        if (ended && !terminator) return false;
        if (terminator) ended = true;
    }
    return *name && strcmp(name, ".") && strcmp(name, "..") && !strpbrk(name, "/\\");
}

bool rapi_list(rapi_t *rapi, const char *pattern, rapi_file_t **files, size_t *count) {
    *files = NULL;
    *count = 0;
    message_t message = { 0 };
    uint32_t found;
    uint32_t flags = FIND_ATTRIBUTES | FIND_LAST_WRITE_TIME | FIND_SIZE_LOW | FIND_NAME;
    if (!message_begin(&message, COMMAND_FIND_ALL_FILES) || !message_string(&message, pattern) || !message_u32(&message, flags)) return false;
    if (!call(rapi, &message) || !reply_u32(rapi, &found)) return false;
    if (!found) return true;
    if (found > (rapi->reply_length - rapi->reply_offset) / FIND_ENTRY_MIN) {
        set_error(rapi, "short reply from the " RAPI_DEVICE);
        return false;
    }
    rapi_file_t *list = calloc(found, sizeof *list);
    if (!list) return false;
    size_t kept = 0;
    for (uint32_t i = 0; i < found; i++) {
        rapi_file_t *file = &list[kept];
        uint32_t name_units, low, high;
        if (!reply_u32(rapi, &name_units) || !reply_u32(rapi, &file->attributes) || !reply_u32(rapi, &low) ||
            !reply_u32(rapi, &high) || !reply_u32(rapi, &file->size)) {
            free(list);
            return false;
        }
        file->write_time = (uint64_t)high << 32 | low;
        const uint8_t *name = reply_bytes(rapi, (size_t)name_units * 2);
        if (!name) {
            free(list);
            return false;
        }
        utf16_to_utf8(name, name_units, file->name, sizeof file->name);
        if (safe_file_name(name, name_units, file->name)) kept++;
    }
    if (!kept) {
        free(list);
        return true;
    }
    *files = list;
    *count = kept;
    return true;
}

bool rapi_stat(rapi_t *rapi, const char *path, rapi_file_t *file) {
    rapi_file_t *files;
    size_t count;
    if (!rapi_list(rapi, path, &files, &count)) return false;
    if (count != 1) {
        free(files);
        return ce_failed(rapi, "find", path, 2);
    }
    *file = files[0];
    free(files);
    return true;
}

static bool path_call(rapi_t *rapi, uint32_t command, const char *what, const char *path, const char *second) {
    message_t message = { 0 };
    uint32_t last_error, result;
    if (!message_begin(&message, command) || !message_optional_string(&message, path)) return false;
    if (second && !message_optional_string(&message, second)) return false;
    if (command == COMMAND_CREATE_DIRECTORY && !message_u32(&message, 0)) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    return result ? true : ce_failed(rapi, what, path, last_error);
}

bool rapi_delete(rapi_t *rapi, const char *path) {
    return path_call(rapi, COMMAND_DELETE_FILE, "delete", path, NULL);
}

bool rapi_make_directory(rapi_t *rapi, const char *path) {
    return path_call(rapi, COMMAND_CREATE_DIRECTORY, "create folder", path, NULL);
}

bool rapi_remove_directory(rapi_t *rapi, const char *path) {
    return path_call(rapi, COMMAND_REMOVE_DIRECTORY, "remove folder", path, NULL);
}

bool rapi_move(rapi_t *rapi, const char *from, const char *to) {
    return path_call(rapi, COMMAND_MOVE_FILE, "move", from, to);
}

bool rapi_run(rapi_t *rapi, const char *program, const char *arguments) {
    message_t message = { 0 };
    uint32_t last_error, result;
    if (!message_begin(&message, COMMAND_CREATE_PROCESS) || !message_optional_string(&message, program) ||
        !message_optional_string(&message, arguments && *arguments ? arguments : NULL)) return false;
    for (int i = 0; i < 7; i++) message_u32(&message, 0);
    if (!message_optional_out(&message, 16) || !call_result(rapi, &message, &last_error, &result)) return false;
    return result ? true : ce_failed(rapi, "run", program, last_error);
}

static bool reply_optional(rapi_t *rapi, const uint8_t **data, uint32_t *size) {
    uint32_t present, has_value = 0;
    *data = NULL;
    *size = 0;
    if (!reply_u32(rapi, &present)) return false;
    if (present != 1) return true;
    if (!reply_u32(rapi, size) || !reply_u32(rapi, &has_value)) return false;
    if (has_value != 1) return true;
    *data = reply_bytes(rapi, *size);
    return *data != NULL;
}

static uint32_t decode_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool message_optional_u32(message_t *message, uint32_t value) {
    return message_u32(message, 1) && message_u32(message, 4) && message_u32(message, 1) && message_u32(message, value);
}

static bool reg_failed(rapi_t *rapi, const char *what, const char *name, uint32_t code) {
    set_error(rapi, "registry %s %s: %s (%u)", what, name, error_name(code), code);
    return false;
}

bool rapi_reg_open(rapi_t *rapi, uint32_t parent, const char *subkey, bool create, uint32_t *key) {
    message_t message = { 0 };
    uint32_t last_error, result;
    if (!message_begin(&message, create ? COMMAND_REG_CREATE_KEY : COMMAND_REG_OPEN_KEY) || !message_u32(&message, parent) ||
        !message_string(&message, subkey) || (create && !message_string(&message, ""))) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    if (result != 0) return reg_failed(rapi, create ? "create" : "open", subkey, result);
    return reply_u32(rapi, key);
}

bool rapi_reg_close(rapi_t *rapi, uint32_t key) {
    if (rapi->os_major >= 2) return true;
    message_t message = { 0 };
    uint32_t last_error, result;
    return message_begin(&message, COMMAND_REG_CLOSE_KEY) && message_u32(&message, key) &&
           call_result(rapi, &message, &last_error, &result) && result == 0;
}

bool rapi_reg_subkey(rapi_t *rapi, uint32_t key, uint32_t index, char *name, size_t size, bool *found) {
    message_t message = { 0 };
    uint32_t last_error, result, units = RAPI_NAME_MAX / 3;
    *found = false;
    if (!message_begin(&message, COMMAND_REG_ENUM_KEY) || !message_u32(&message, key) || !message_u32(&message, index) ||
        !message_optional_out(&message, units * 2) || !message_optional_u32(&message, units) || !message_u32(&message, 0) ||
        !message_u32(&message, 0) || !message_u32(&message, 0) || !message_u32(&message, 0)) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    if (result == ERROR_NO_MORE_ITEMS) return true;
    if (result != 0) return reg_failed(rapi, "list", "", result);
    const uint8_t *data;
    uint32_t length;
    if (!reply_optional(rapi, &data, &length)) return false;
    utf16_to_utf8(data ? data : (const uint8_t *)"\0\0", data ? length / 2 : 1, name, size);
    *found = true;
    return true;
}

bool rapi_reg_value(rapi_t *rapi, uint32_t key, uint32_t index, char *name, size_t size, uint32_t *type, uint8_t *data, uint32_t *length, bool *found) {
    message_t message = { 0 };
    uint32_t last_error, result, units = RAPI_NAME_MAX / 3;
    *found = false;
    if (!message_begin(&message, COMMAND_REG_ENUM_VALUE) || !message_u32(&message, key) || !message_u32(&message, index) ||
        !message_optional_out(&message, units * 2) || !message_optional_u32(&message, units) || !message_u32(&message, 0) ||
        !message_optional_out(&message, 4) || !message_optional_out(&message, RAPI_REG_DATA_MAX) ||
        !message_optional_u32(&message, RAPI_REG_DATA_MAX)) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    if (result == ERROR_NO_MORE_ITEMS) return true;
    if (result != 0) return reg_failed(rapi, "list", "", result);
    const uint8_t *name_data, *type_data, *value_data, *size_data;
    uint32_t name_length, type_length, value_length, size_length;
    if (!reply_optional(rapi, &name_data, &name_length) || !reply_optional(rapi, &size_data, &size_length) ||
        !reply_optional(rapi, &type_data, &type_length) || !reply_optional(rapi, &value_data, &value_length) ||
        !reply_optional(rapi, &size_data, &size_length)) return false;
    utf16_to_utf8(name_data ? name_data : (const uint8_t *)"\0\0", name_data ? name_length / 2 : 1, name, size);
    *type = type_data && type_length >= 4 ? decode_u32(type_data) : 0;
    uint32_t actual = size_data && size_length >= 4 ? decode_u32(size_data) : value_length;
    if (actual > value_length) actual = value_length;
    if (actual > RAPI_REG_DATA_MAX) actual = RAPI_REG_DATA_MAX;
    if (value_data) memcpy(data, value_data, actual);
    *length = value_data ? actual : 0;
    *found = true;
    return true;
}

bool rapi_reg_get(rapi_t *rapi, uint32_t key, const char *name, uint32_t *type, uint8_t *data, uint32_t capacity, uint32_t *length) {
    message_t message = { 0 };
    uint32_t last_error, result;
    if (!message_begin(&message, COMMAND_REG_QUERY_VALUE) || !message_u32(&message, key) || !message_optional_string(&message, name) ||
        !message_u32(&message, 0) || !message_optional_out(&message, 4) || !message_optional_out(&message, RAPI_REG_DATA_MAX) ||
        !message_optional_u32(&message, RAPI_REG_DATA_MAX)) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    if (result != 0) return reg_failed(rapi, "read", name, result);
    const uint8_t *type_data, *value_data, *size_data;
    uint32_t type_length, value_length, size_length;
    if (!reply_optional(rapi, &type_data, &type_length) || !reply_optional(rapi, &value_data, &value_length) ||
        !reply_optional(rapi, &size_data, &size_length)) return false;
    *type = type_data && type_length >= 4 ? decode_u32(type_data) : 0;
    uint32_t actual = size_data && size_length >= 4 ? decode_u32(size_data) : value_length;
    if (actual > value_length) actual = value_length;
    if (actual > capacity) actual = capacity;
    if (value_data) memcpy(data, value_data, actual);
    *length = value_data ? actual : 0;
    return true;
}

bool rapi_reg_set(rapi_t *rapi, uint32_t key, const char *name, uint32_t type, const uint8_t *data, uint32_t length) {
    message_t message = { 0 };
    uint32_t last_error, result;
    if (!message_begin(&message, COMMAND_REG_SET_VALUE) || !message_u32(&message, key) || !message_optional_string(&message, name) ||
        !message_u32(&message, type) || !message_u32(&message, 1) || !message_u32(&message, length) || !message_u32(&message, 1) ||
        !message_bytes(&message, data, length) || !message_u32(&message, length)) return false;
    if (!call_result(rapi, &message, &last_error, &result)) return false;
    return result == 0 ? true : reg_failed(rapi, "write", name, result);
}

void rapi_reg_text(const uint8_t *data, uint32_t length, char *out, size_t size) {
    utf16_to_utf8(data, length / 2, out, size);
}

uint32_t rapi_reg_encode(const char *text, uint8_t *out, size_t size) {
    uint8_t bytes[RAPI_NAME_MAX * 2];
    size_t length = encode_utf16(text, bytes) * 2;
    if (length > size) length = size;
    memcpy(out, bytes, length);
    return (uint32_t)length;
}

static bool open_file(rapi_t *rapi, const char *path, bool write, uint32_t *handle) {
    message_t message = { 0 };
    uint32_t last_error;
    if (!message_begin(&message, COMMAND_CREATE_FILE) || !message_u32(&message, write ? GENERIC_WRITE : GENERIC_READ) ||
        !message_u32(&message, 0) || !message_u32(&message, write ? CREATE_ALWAYS : OPEN_EXISTING) ||
        !message_u32(&message, ATTRIBUTE_NORMAL) || !message_u32(&message, 0) || !message_string(&message, path)) return false;
    if (!call(rapi, &message) || !reply_u32(rapi, &last_error) || !reply_u32(rapi, handle)) return false;
    return *handle != INVALID_HANDLE ? true : ce_failed(rapi, write ? "create" : "open", path, last_error);
}

static bool close_file(rapi_t *rapi, uint32_t handle) {
    message_t message = { 0 };
    uint32_t last_error, result;
    return message_begin(&message, COMMAND_CLOSE_HANDLE) && message_u32(&message, handle) &&
           call_result(rapi, &message, &last_error, &result) && result;
}

bool rapi_put(rapi_t *rapi, const char *remote, const void *data, size_t length) {
    uint32_t handle, last_error, result, written;
    if (!open_file(rapi, remote, true, &handle)) return false;
    message_t message = { 0 };
    bool success = message_begin(&message, COMMAND_WRITE_FILE) && message_u32(&message, handle) && message_u32(&message, 1) &&
                   message_u32(&message, (uint32_t)length) && message_bytes(&message, data, length) && message_u32(&message, 0) &&
                   call_result(rapi, &message, &last_error, &result) && reply_u32(rapi, &written);
    if (success && (!result || written != length)) success = ce_failed(rapi, "write", remote, last_error);
    bool closed = close_file(rapi, handle);
    return success && closed;
}

bool rapi_upload(rapi_t *rapi, const char *local, const char *remote, rapi_progress_fn progress, void *context) {
    FILE *file = fopen(local, "rb");
    if (!file) {
        set_error(rapi, "can't read %s", local);
        return false;
    }
    struct stat info;
    uint64_t total = fstat(fileno(file), &info) == 0 ? (uint64_t)info.st_size : 0;
    uint32_t handle, last_error;
    if (!open_file(rapi, remote, true, &handle)) {
        fclose(file);
        return false;
    }
    bool success = true;
    uint64_t done = 0;
    uint8_t chunk[CHUNK_SIZE];
    size_t got;
    if (progress) progress(context, 0, total);
    while (success && (got = fread(chunk, 1, sizeof chunk, file)) > 0) {
        message_t message = { 0 };
        uint32_t result, written;
        success = message_begin(&message, COMMAND_WRITE_FILE) && message_u32(&message, handle) && message_u32(&message, 1) &&
                  message_u32(&message, (uint32_t)got) && message_bytes(&message, chunk, got) && message_u32(&message, 0) &&
                  call_result(rapi, &message, &last_error, &result) && reply_u32(rapi, &written);
        if (success && (!result || written != got)) success = ce_failed(rapi, "write", remote, last_error);
        done += got;
        if (success && progress) progress(context, done, total);
    }
    fclose(file);
    bool closed = close_file(rapi, handle);
    return success && closed;
}

bool rapi_download(rapi_t *rapi, const char *remote, const char *local, rapi_progress_fn progress, void *context) {
    char partial[1100];
    int partial_length = snprintf(partial, sizeof partial, "%s.part", local);
    if (partial_length < 0 || (size_t)partial_length >= sizeof partial) {
        set_error(rapi, "path too long: %s", local);
        return false;
    }
    rapi_file_t info;
    if (!rapi_stat(rapi, remote, &info)) return false;
    uint32_t handle, last_error;
    if (!open_file(rapi, remote, false, &handle)) return false;
    FILE *file = fopen(partial, "wb");
    if (!file) {
        close_file(rapi, handle);
        set_error(rapi, "can't write %s", partial);
        return false;
    }
    bool success = true;
    uint64_t done = 0;
    if (progress) progress(context, 0, info.size);
    for (;;) {
        message_t message = { 0 };
        uint32_t result, got;
        success = message_begin(&message, COMMAND_READ_FILE) && message_u32(&message, handle) &&
                  message_optional_out(&message, CHUNK_SIZE) && message_u32(&message, 0) &&
                  call_result(rapi, &message, &last_error, &result) && reply_u32(rapi, &got);
        if (success && !result) success = ce_failed(rapi, "read", remote, last_error);
        if (!success || got == 0) break;
        const uint8_t *data = reply_bytes(rapi, got);
        if (!data || fwrite(data, 1, got, file) != got) {
            success = false;
            if (data) set_error(rapi, "can't write %s", partial);
            break;
        }
        done += got;
        if (progress) progress(context, done, info.size);
    }
    bool closed = close_file(rapi, handle);
    if (fclose(file) != 0) success = false;
    if (success && closed && rename(partial, local) == 0) return true;
    unlink(partial);
    return false;
}

#include "net/web_proxy.h"
#include "net/web_image.h"

#include <ctype.h>
#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <zlib.h>

#define HEAD_MAX         16384
#define BODY_MAX         (1024 * 1024)
#define RESPONSE_MAX     (16 * 1024 * 1024)
#define URL_MAX          4096
#define USER_AGENT_MAX   256
#define SOCKET_TIMEOUT   60
#define CONNECT_TIMEOUT  15
#define TRANSFER_TIMEOUT 60
#define HINT_COUNT       512
#define CONNECTION_MAX   8
#define GZIP_MIN         256

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

struct web_proxy {
    net_gateway_log_fn log;
    char user_agent[USER_AGENT_MAX];
    int listener;
    struct sockaddr_un address;
};

typedef struct {
    char  *data;
    size_t length, capacity;
} buffer_t;

typedef struct {
    char method[16];
    char url[URL_MAX];
    char    *headers;
    buffer_t body;
} request_t;

typedef struct {
    CURLcode result;
    long status;
    buffer_t headers;
    buffer_t body;
    char redirect[URL_MAX];
} response_t;

typedef struct {
    net_gateway_log_fn log;
    char user_agent[USER_AGENT_MAX];
    int client;
} connection_t;

typedef struct {
    char url[URL_MAX];
    int width, height;
} image_hint_t;

static image_hint_t hints[HINT_COUNT];
static int next_hint = 0;
static pthread_mutex_t hint_lock = PTHREAD_MUTEX_INITIALIZER;
static int active_connections = 0;
static pthread_mutex_t connection_lock = PTHREAD_MUTEX_INITIALIZER;

static void proxy_log(net_gateway_log_fn log, const char *format, ...) {
    if (!log) return;
    char message[URL_MAX + 256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof message, format, args);
    va_end(args);
    log(message);
}

static bool buffer_append(buffer_t *buffer, const void *data, size_t length) {
    if (buffer->length + length > RESPONSE_MAX) return false;
    if (buffer->length + length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->length + length + 1) capacity *= 2;
        char *grown = realloc(buffer->data, capacity);
        if (!grown) return false;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    if (length) memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = 0;
    return true;
}

static void buffer_printf(buffer_t *buffer, const char *format, ...) {
    char text[URL_MAX + 256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (length > 0) buffer_append(buffer, text, (size_t)length < sizeof text ? (size_t)length : sizeof text - 1);
}

static void buffer_free(buffer_t *buffer) {
    free(buffer->data);
    *buffer = (buffer_t){ 0 };
}

static void buffer_replace(buffer_t *buffer, buffer_t *replacement) {
    buffer_free(buffer);
    *buffer = *replacement;
    *replacement = (buffer_t){ 0 };
}

static const char *find_nocase(const char *start, const char *end, const char *needle) {
    size_t length = strlen(needle);
    for (const char *p = start; p + length <= end; p++) {
        if (!strncasecmp(p, needle, length)) return p;
    }
    return NULL;
}

static bool next_line(const char **cursor, char *line, size_t max) {
    const char *start = *cursor;
    if (!start || !*start) return false;
    const char *end = strstr(start, "\r\n");
    size_t length = end ? (size_t)(end - start) : strlen(start);
    if (length == 0) return false;
    if (length >= max) length = max - 1;
    memcpy(line, start, length);
    line[length] = 0;
    *cursor = end ? end + 2 : start + strlen(start);
    return true;
}

static bool header_named(const char *line, const char *name) {
    size_t length = strlen(name);
    return !strncasecmp(line, name, length) && line[length] == ':';
}

static const char *header_value_of(const char *line) {
    const char *value = strchr(line, ':');
    if (!value) return "";
    value++;
    while (*value == ' ' || *value == '\t') value++;
    return value;
}

static bool find_header(const char *block, const char *name, char *out, size_t max) {
    char line[URL_MAX];
    const char *cursor = block;
    bool found = false;
    while (next_line(&cursor, line, sizeof line)) {
        if (!header_named(line, name)) continue;
        snprintf(out, max, "%s", header_value_of(line));
        found = true;
    }
    return found;
}

static bool send_all(int client, const char *data, size_t length) {
    while (length) {
        ssize_t sent = send(client, data, length, SEND_FLAGS);
        if (sent <= 0) return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

static bool read_request(int client, request_t *request) {
    char head[HEAD_MAX + 1];
    size_t length = 0;
    char *end = NULL;
    while (!end) {
        if (length == HEAD_MAX) return false;
        ssize_t got = recv(client, head + length, HEAD_MAX - length, 0);
        if (got <= 0) return false;
        length += (size_t)got;
        head[length] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    size_t head_length = (size_t)(end - head) + 4;
    if (sscanf(head, "%15s %4095s", request->method, request->url) != 2) return false;

    char *headers = strstr(head, "\r\n") + 2;
    size_t headers_length = head_length - (size_t)(headers - head);
    request->headers = malloc(headers_length + 1);
    memcpy(request->headers, headers, headers_length);
    request->headers[headers_length] = 0;

    char value[64];
    size_t content_length = 0;
    if (find_header(request->headers, "Content-Length", value, sizeof value)) content_length = strtoul(value, NULL, 10);
    if (content_length > BODY_MAX) return false;
    size_t already = length - head_length;
    if (already > content_length) already = content_length;
    buffer_append(&request->body, head + head_length, already);
    while (request->body.length < content_length) {
        char chunk[4096];
        size_t wanted = content_length - request->body.length;
        ssize_t got = recv(client, chunk, wanted < sizeof chunk ? wanted : sizeof chunk, 0);
        if (got <= 0) return false;
        buffer_append(&request->body, chunk, (size_t)got);
    }
    return true;
}

static struct curl_slist *upstream_headers(const char *block, const char *user_agent) {
    static const char *const dropped[] = {
        "Host", "Connection", "Proxy-Connection", "Keep-Alive", "Proxy-Authorization",
        "Accept-Encoding", "Content-Length", "Transfer-Encoding", "Expect", "TE", "Upgrade",
    };
    struct curl_slist *list = NULL;
    char line[URL_MAX];
    const char *cursor = block;
    while (next_line(&cursor, line, sizeof line)) {
        bool keep = strchr(line, ':') != NULL;
        for (size_t i = 0; keep && i < sizeof dropped / sizeof dropped[0]; i++) {
            if (header_named(line, dropped[i])) keep = false;
        }
        if (keep && *user_agent && header_named(line, "User-Agent")) keep = false;
        if (keep) list = curl_slist_append(list, line);
    }
    if (*user_agent) {
        snprintf(line, sizeof line, "User-Agent: %s", user_agent);
        list = curl_slist_append(list, line);
    }
    return curl_slist_append(list, "Expect:");
}

static size_t on_header(char *data, size_t size, size_t count, void *opaque) {
    response_t *response = opaque;
    size_t length = size * count;
    if (length >= 5 && !strncmp(data, "HTTP/", 5)) response->headers.length = 0;
    buffer_append(&response->headers, data, length);
    return length;
}

static size_t on_body(char *data, size_t size, size_t count, void *opaque) {
    response_t *response = opaque;
    return buffer_append(&response->body, data, size * count) ? size * count : 0;
}

static void fetch(const request_t *request, struct curl_slist *headers, const char *url, response_t *response) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        response->result = CURLE_FAILED_INIT;
        return;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)CONNECT_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)TRANSFER_TIMEOUT);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, response);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    if (!strcmp(request->method, "HEAD")) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else if (!strcmp(request->method, "POST")) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request->body.length);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body.data ? request->body.data : "");
    } else if (strcmp(request->method, "GET")) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, request->method);
    }
    response->result = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
    char *redirect = NULL;
    if (curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect) == CURLE_OK && redirect) {
        snprintf(response->redirect, sizeof response->redirect, "%s", redirect);
    }
    curl_easy_cleanup(curl);
}

static void response_free(response_t *response) {
    buffer_free(&response->headers);
    buffer_free(&response->body);
    *response = (response_t){ 0 };
}

static bool opening_tag(const char *p, const char *end, const char *name) {
    size_t length = strlen(name);
    if ((size_t)(end - p) < length + 2 || p[0] != '<' || strncasecmp(p + 1, name, length)) return false;
    char next = p[1 + length];
    return next == '>' || next == '/' || isspace((unsigned char)next);
}

static void strip_markup(buffer_t *body) {
    static const char *const elements[] = { "script", "style", "svg" };
    buffer_t out = { 0 };
    const char *p = body->data, *end = body->data + body->length;
    while (p < end) {
        const char *skip_to = NULL;
        if (*p == '<') {
            if (end - p >= 4 && !memcmp(p, "<!--", 4)) {
                const char *close = find_nocase(p + 4, end, "-->");
                skip_to = close ? close + 3 : end;
            }
            if (!skip_to && opening_tag(p, end, "meta")) {
                const char *tag_end = memchr(p, '>', (size_t)(end - p));
                if (tag_end && find_nocase(p, tag_end, "content-type")) skip_to = tag_end + 1;
            }
            for (size_t i = 0; !skip_to && i < sizeof elements / sizeof elements[0]; i++) {
                if (!opening_tag(p, end, elements[i])) continue;
                const char *tag_end = memchr(p, '>', (size_t)(end - p));
                if (tag_end && tag_end[-1] == '/') {
                    skip_to = tag_end + 1;
                    break;
                }
                char closing[16];
                snprintf(closing, sizeof closing, "</%s", elements[i]);
                const char *close = find_nocase(p, end, closing);
                const char *close_end = close ? memchr(close, '>', (size_t)(end - close)) : NULL;
                skip_to = close_end ? close_end + 1 : end;
            }
        }
        if (skip_to) {
            p = skip_to;
            continue;
        }
        const char *next = memchr(p + 1, '<', (size_t)(end - p - 1));
        if (!next) next = end;
        buffer_append(&out, p, (size_t)(next - p));
        p = next;
    }
    buffer_replace(body, &out);
}

static void rewrite_secure_links(buffer_t *body) {
    buffer_t out = { 0 };
    const char *p = body->data, *end = body->data + body->length;
    while (p < end) {
        const char *found = find_nocase(p, end, "https://");
        if (!found) {
            buffer_append(&out, p, (size_t)(end - p));
            break;
        }
        buffer_append(&out, p, (size_t)(found - p));
        buffer_append(&out, "http://", 7);
        p = found + 8;
    }
    buffer_replace(body, &out);
}

static size_t decode_utf8(const unsigned char *p, const unsigned char *end, uint32_t *codepoint) {
    size_t length;
    uint32_t value;
    if (p[0] < 0x80) { *codepoint = p[0]; return 1; }
    if ((p[0] & 0xE0) == 0xC0) { length = 2; value = p[0] & 0x1F; }
    else if ((p[0] & 0xF0) == 0xE0) { length = 3; value = p[0] & 0x0F; }
    else if ((p[0] & 0xF8) == 0xF0) { length = 4; value = p[0] & 0x07; }
    else return 0;
    if ((size_t)(end - p) < length) return 0;
    for (size_t i = 1; i < length; i++) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        value = value << 6 | (p[i] & 0x3F);
    }
    if ((length == 2 && value < 0x80) || (length == 3 && value < 0x800) || (length == 4 && value < 0x10000) || value > 0x10FFFF) return 0;
    *codepoint = value;
    return length;
}

static int windows_1252(uint32_t codepoint) {
    static const uint16_t high[32] = {
        0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
        0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
    };
    if (codepoint < 0x80 || (codepoint >= 0xA0 && codepoint <= 0xFF)) return (int)codepoint;
    for (int i = 0; i < 32; i++) {
        if (high[i] && high[i] == codepoint) return 0x80 + i;
    }
    if ((codepoint >= 0x200B && codepoint <= 0x200F) || codepoint == 0x2060 || codepoint == 0xFEFF) return -1;
    if (codepoint >= 0x2000 && codepoint <= 0x200A) return ' ';
    if (codepoint >= 0x2010 && codepoint <= 0x2012) return '-';
    if (codepoint == 0x2032) return '\'';
    if (codepoint == 0x2033) return '"';
    return '?';
}

static void utf8_to_windows_1252(buffer_t *body) {
    const unsigned char *start = (const unsigned char *)body->data, *end = start + body->length;
    bool non_ascii = false;
    for (const unsigned char *p = start; p < end;) {
        uint32_t codepoint;
        size_t length = decode_utf8(p, end, &codepoint);
        if (!length) return;
        if (length > 1) non_ascii = true;
        p += length;
    }
    if (!non_ascii) return;
    buffer_t out = { 0 };
    for (const unsigned char *p = start; p < end;) {
        uint32_t codepoint;
        p += decode_utf8(p, end, &codepoint);
        int byte = windows_1252(codepoint);
        if (byte >= 0) {
            char converted = (char)byte;
            buffer_append(&out, &converted, 1);
        }
    }
    buffer_replace(body, &out);
}

static bool attribute(const char *tag, const char *tag_end, const char *name, char *out, size_t size) {
    size_t length = strlen(name);
    for (const char *p = tag + 1; p + length < tag_end; p++) {
        if (!isspace((unsigned char)p[-1]) || strncasecmp(p, name, length)) continue;
        const char *value = p + length;
        while (value < tag_end && isspace((unsigned char)*value)) value++;
        if (value >= tag_end || *value != '=') continue;
        value++;
        while (value < tag_end && isspace((unsigned char)*value)) value++;
        char quote = (*value == '"' || *value == '\'') ? *value++ : 0;
        const char *end = value;
        while (end < tag_end && (quote ? *end != quote : !isspace((unsigned char)*end) && *end != '>')) end++;
        size_t copied = 0;
        for (const char *c = value; c < end && copied + 1 < size; c++) {
            if (!strncmp(c, "&amp;", 5) && c + 5 <= end) {
                out[copied++] = '&';
                c += 4;
            } else {
                out[copied++] = *c;
            }
        }
        out[copied] = 0;
        return true;
    }
    return false;
}

static void resolve_url(const char *base, const char *reference, char *out, size_t size) {
    if (!strncasecmp(reference, "https://", 8)) {
        snprintf(out, size, "http://%s", reference + 8);
    } else if (!strncasecmp(reference, "http://", 7)) {
        snprintf(out, size, "%s", reference);
    } else if (!strncmp(reference, "//", 2)) {
        snprintf(out, size, "http:%s", reference);
    } else {
        const char *authority = base + 7;
        size_t origin = (size_t)(authority - base) + strcspn(authority, "/?#");
        if (reference[0] == '/') {
            snprintf(out, size, "%.*s%s", (int)origin, base, reference);
        } else {
            size_t directory = origin;
            for (size_t i = origin; base[i] && base[i] != '?' && base[i] != '#'; i++) {
                if (base[i] == '/') directory = i + 1;
            }
            if (directory == origin) snprintf(out, size, "%.*s/%s", (int)origin, base, reference);
            else snprintf(out, size, "%.*s%s", (int)directory, base, reference);
        }
    }
    out[strcspn(out, "#")] = 0;
    char *path = strchr(out + 7, '/');
    if (!path) return;
    char *write = path;
    for (char *read = path; *read;) {
        if (!strncmp(read, "/./", 3)) {
            read += 2;
        } else if (!strncmp(read, "/../", 4)) {
            read += 3;
            while (write > path && *--write != '/') continue;
        } else if (*read == '?') {
            memmove(write, read, strlen(read) + 1);
            return;
        } else {
            *write++ = *read++;
        }
    }
    *write = 0;
}

static int dimension(const char *text) {
    char *end;
    long value = strtol(text, &end, 10);
    return value > 0 && value < 10000 && *end != '%' ? (int)value : 0;
}

static void record_image_hints(const char *page_url, const buffer_t *body) {
    const char *p = body->data, *end = body->data + body->length;
    while ((p = find_nocase(p, end, "<img")) != NULL) {
        const char *tag_end = memchr(p, '>', (size_t)(end - p));
        if (!tag_end) break;
        char source[URL_MAX], width[32], height[32];
        int hint_width = attribute(p, tag_end, "width", width, sizeof width) ? dimension(width) : 0;
        int hint_height = attribute(p, tag_end, "height", height, sizeof height) ? dimension(height) : 0;
        if (attribute(p, tag_end, "src", source, sizeof source) && *source && strncasecmp(source, "data:", 5) && (hint_width || hint_height)) {
            pthread_mutex_lock(&hint_lock);
            image_hint_t *hint = &hints[next_hint];
            next_hint = (next_hint + 1) % HINT_COUNT;
            resolve_url(page_url, source, hint->url, sizeof hint->url);
            hint->width = hint_width;
            hint->height = hint_height;
            pthread_mutex_unlock(&hint_lock);
        }
        p = tag_end;
    }
}

static void find_image_hint(const char *url, int *width, int *height) {
    *width = *height = 0;
    pthread_mutex_lock(&hint_lock);
    for (int i = 0; i < HINT_COUNT; i++) {
        if (!strcmp(hints[i].url, url)) {
            *width = hints[i].width;
            *height = hints[i].height;
        }
    }
    pthread_mutex_unlock(&hint_lock);
}

static long legacy_status(long status) {
    if (status == 303 || status == 307) return 302;
    if (status == 308) return 301;
    return status;
}

static const char *reason_phrase(long status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 301: return "Moved Permanently";
    case 302: return "Moved Temporarily";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    }
    return status < 300 ? "OK" : status < 400 ? "Redirect" : "Error";
}

static void send_error(int client, long status, const char *detail) {
    buffer_t page = { 0 };
    buffer_printf(&page, "<html><head><title>%ld %s</title></head><body><h1>%s</h1><p>%s</p></body></html>\r\n",
                  status, reason_phrase(status), reason_phrase(status), detail);
    buffer_t head = { 0 };
    buffer_printf(&head, "HTTP/1.0 %ld %s\r\nContent-Type: text/html\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                  status, reason_phrase(status), page.length);
    send_all(client, head.data, head.length);
    send_all(client, page.data, page.length);
    buffer_free(&head);
    buffer_free(&page);
}

static void strip_secure_attribute(char *cookie) {
    char *p = cookie;
    while ((p = strchr(p, ';'))) {
        char *attribute = p + 1;
        while (*attribute == ' ') attribute++;
        size_t length = strcspn(attribute, ";");
        while (length && attribute[length - 1] == ' ') length--;
        if (length == 6 && !strncasecmp(attribute, "secure", 6)) {
            memmove(p, attribute + length, strlen(attribute + length) + 1);
        } else {
            p++;
        }
    }
}

static void browser_location(const char *location, char *out, size_t size) {
    if (!strncasecmp(location, "https://", 8)) snprintf(out, size, "http://%s", location + 8);
    else snprintf(out, size, "%s", location);
}

static void replace_with_moved_page(response_t *response, const char *location) {
    buffer_t page = { 0 };
    buffer_printf(&page, "<HTML><HEAD><TITLE>Moved</TITLE></HEAD><BODY>The document has moved <A HREF=\"");
    for (const char *at = location; *at; at++) {
        if (*at == '&') buffer_printf(&page, "&amp;");
        else if (*at == '"') buffer_printf(&page, "&quot;");
        else buffer_append(&page, at, 1);
    }
    buffer_printf(&page, "\">here</A>.</BODY></HTML>");
    buffer_free(&response->body);
    response->body = page;
}

static bool accepts_gzip(const request_t *request) {
    char value[256];
    if (!find_header(request->headers, "Accept-Encoding", value, sizeof value)) return false;
    return find_nocase(value, value + strlen(value), "gzip") != NULL;
}

static bool gzip_body(buffer_t *body) {
    z_stream stream = { 0 };
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return false;
    buffer_t out = { 0 };
    uLong bound = deflateBound(&stream, (uLong)body->length);
    out.data = malloc(bound + 1);
    if (!out.data) {
        deflateEnd(&stream);
        return false;
    }
    out.capacity = bound + 1;
    stream.next_in = (Bytef *)body->data;
    stream.avail_in = (uInt)body->length;
    stream.next_out = (Bytef *)out.data;
    stream.avail_out = (uInt)bound;
    int result = deflate(&stream, Z_FINISH);
    out.length = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        buffer_free(&out);
        return false;
    }
    buffer_replace(body, &out);
    return true;
}

static bool looks_like_html(const buffer_t *body) {
    size_t at = 0;
    while (at < body->length && isspace((unsigned char)body->data[at])) at++;
    return body->length - at >= 5 && (!strncasecmp(body->data + at, "<!doc", 5) || !strncasecmp(body->data + at, "<html", 5));
}

static void send_response(int client, const request_t *request, response_t *response) {
    static const char *const passed[] = { "Last-Modified", "Expires", "Content-Disposition", "WWW-Authenticate" };
    char content_type[256] = "", mime[128] = "";
    find_header(response->headers.data ? response->headers.data : "", "Content-Type", content_type, sizeof content_type);
    size_t mime_length = strcspn(content_type, "; ");
    if (mime_length >= sizeof mime) mime_length = sizeof mime - 1;
    for (size_t i = 0; i < mime_length; i++) mime[i] = (char)tolower((unsigned char)content_type[i]);
    mime[mime_length] = 0;
    char location[URL_MAX] = "";
    if (*response->redirect) browser_location(response->redirect, location, sizeof location);
    bool untyped = !*mime || !strcmp(mime, "application/octet-stream") || !strcmp(mime, "application/binary");
    if (*location && response->status >= 300 && response->status < 400) {
        replace_with_moved_page(response, location);
        snprintf(mime, sizeof mime, "text/html");
    } else if (!strcmp(mime, "application/xhtml+xml") || (untyped && looks_like_html(&response->body))) {
        snprintf(mime, sizeof mime, "text/html");
    }
    if (!strcmp(mime, "text/html")) snprintf(content_type, sizeof content_type, "text/html");
    bool text = !strncmp(mime, "text/", 5);

    if (!strcmp(mime, "text/html")) {
        strip_markup(&response->body);
        rewrite_secure_links(&response->body);
        record_image_hints(request->url, &response->body);
    }
    if (text) utf8_to_windows_1252(&response->body);
    uint8_t *gif;
    size_t gif_length;
    int hint_width, hint_height;
    bool image = !strncmp(mime, "image/", 6);
    find_image_hint(request->url, &hint_width, &hint_height);
    if (image && response->body.length &&
        web_image_convert((const uint8_t *)response->body.data, response->body.length, !strcmp(mime, "image/svg+xml"),
                          hint_width, hint_height, &gif, &gif_length)) {
        free(response->body.data);
        response->body = (buffer_t){ (char *)gif, gif_length, gif_length };
        snprintf(content_type, sizeof content_type, "image/gif");
    }
    bool gzipped = !text && !image && !*location && strcmp(request->method, "HEAD") && response->body.length >= GZIP_MIN &&
                   accepts_gzip(request) && gzip_body(&response->body);

    long status = legacy_status(response->status);
    buffer_t head = { 0 };
    buffer_printf(&head, "HTTP/1.0 %ld %s\r\n", status, reason_phrase(status));
    if (*content_type) buffer_printf(&head, "Content-Type: %s\r\n", text ? mime : content_type);
    if (*location) buffer_printf(&head, "Location: %s\r\n", location);
    if (gzipped) buffer_printf(&head, "Content-Encoding: gzip\r\n");
    char line[URL_MAX];
    const char *cursor = response->headers.data ? strstr(response->headers.data, "\r\n") : NULL;
    if (cursor) cursor += 2;
    while (next_line(&cursor, line, sizeof line)) {
        if (header_named(line, "Set-Cookie")) {
            strip_secure_attribute(line);
            buffer_printf(&head, "%s\r\n", line);
            continue;
        }
        for (size_t i = 0; i < sizeof passed / sizeof passed[0]; i++) {
            if (header_named(line, passed[i])) buffer_printf(&head, "%s\r\n", line);
        }
    }
    buffer_printf(&head, "Content-Length: %zu\r\nConnection: close\r\n\r\n", response->body.length);
    if (send_all(client, head.data, head.length) && strcmp(request->method, "HEAD") && response->body.length) {
        send_all(client, response->body.data, response->body.length);
    }
    buffer_free(&head);
}

static void handle(connection_t *connection) {
    int client = connection->client;
    request_t request = { 0 };
    if (!read_request(client, &request)) goto done;
    if (!strcmp(request.method, "CONNECT")) {
        send_error(client, 501, "HTTPS through the proxy is not supported. Use http:// addresses; the proxy fetches them over HTTPS.");
        goto done;
    }
    if (strncasecmp(request.url, "http://", 7)) {
        send_error(client, 400, "This is the velo-emu web proxy. Set it as the proxy server in Pocket IE.");
        goto done;
    }

    const char *rest = request.url + 7;
    size_t authority = strcspn(rest, "/?#");
    bool explicit_port = memchr(rest, ':', authority) != NULL;
    struct curl_slist *headers = upstream_headers(request.headers, connection->user_agent);
    response_t response = { 0 };
    const char *scheme = "http";
    if (!explicit_port) {
        char secure_url[URL_MAX + 8];
        snprintf(secure_url, sizeof secure_url, "https://%s", rest);
        fetch(&request, headers, secure_url, &response);
        bool unreachable = response.result != CURLE_OK && response.result != CURLE_WRITE_ERROR &&
                           response.result != CURLE_COULDNT_RESOLVE_HOST;
        bool downgraded = response.result == CURLE_OK && response.status >= 300 && response.status < 400 &&
                          !strcasecmp(response.redirect, request.url);
        if (unreachable || downgraded) response_free(&response);
        else scheme = "https";
    }
    if (!strcmp(scheme, "http")) fetch(&request, headers, request.url, &response);
    curl_slist_free_all(headers);

    if (response.result == CURLE_OK) {
        send_response(client, &request, &response);
        proxy_log(connection->log, "proxy: %s %s -> %ld over %s, %zu bytes\n",
                  request.method, request.url, response.status, scheme, response.body.length);
    } else {
        const char *detail = response.result == CURLE_WRITE_ERROR ? "The response was too large." : curl_easy_strerror(response.result);
        send_error(client, 502, detail);
        proxy_log(connection->log, "proxy: %s %s -> %s\n", request.method, request.url, detail);
    }
    response_free(&response);

done:
    free(request.headers);
    buffer_free(&request.body);
    close(client);
}

static void *connection_thread(void *opaque) {
    connection_t *connection = opaque;
    handle(connection);
    free(connection);
    pthread_mutex_lock(&connection_lock);
    active_connections--;
    pthread_mutex_unlock(&connection_lock);
    return NULL;
}

web_proxy_t *web_proxy_start(net_gateway_log_fn log, const char *user_agent) {
    static bool curl_ready = false;
    if (!curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            proxy_log(log, "proxy: could not initialise libcurl\n");
            return NULL;
        }
        curl_ready = true;
    }
    web_proxy_t *proxy = calloc(1, sizeof *proxy);
    proxy->log = log;
    snprintf(proxy->user_agent, sizeof proxy->user_agent, "%s", user_agent ? user_agent : "");
    proxy->address.sun_family = AF_UNIX;
    if (!net_gateway_socket_path(proxy->address.sun_path, sizeof proxy->address.sun_path, "velo-proxy")) {
        proxy_log(log, "proxy: no usable socket path\n");
        free(proxy);
        return NULL;
    }
    unlink(proxy->address.sun_path);
    proxy->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (proxy->listener < 0 ||
        bind(proxy->listener, (struct sockaddr *)&proxy->address, sizeof proxy->address) != 0 ||
        listen(proxy->listener, 16) != 0) {
        proxy_log(log, "proxy: could not listen on %s: %s\n", proxy->address.sun_path, strerror(errno));
        if (proxy->listener >= 0) close(proxy->listener);
        free(proxy);
        return NULL;
    }
    fcntl(proxy->listener, F_SETFL, fcntl(proxy->listener, F_GETFL) | O_NONBLOCK);
    return proxy;
}

void web_proxy_stop(web_proxy_t *proxy) {
    if (!proxy) return;
    close(proxy->listener);
    unlink(proxy->address.sun_path);
    free(proxy);
}

void web_proxy_poll(web_proxy_t *proxy) {
    if (!proxy) return;
    for (;;) {
        pthread_mutex_lock(&connection_lock);
        bool full = active_connections >= CONNECTION_MAX;
        pthread_mutex_unlock(&connection_lock);
        if (full) return;
        int client = accept(proxy->listener, NULL, NULL);
        if (client < 0) return;
        fcntl(client, F_SETFL, fcntl(client, F_GETFL) & ~O_NONBLOCK);
        struct timeval timeout = { SOCKET_TIMEOUT, 0 };
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
        int enabled = 1;
        setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof enabled);
#endif
        connection_t *connection = malloc(sizeof *connection);
        connection->log = proxy->log;
        memcpy(connection->user_agent, proxy->user_agent, sizeof connection->user_agent);
        connection->client = client;
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        pthread_t thread;
        pthread_mutex_lock(&connection_lock);
        active_connections++;
        pthread_mutex_unlock(&connection_lock);
        if (pthread_create(&thread, &attributes, connection_thread, connection) != 0) {
            pthread_mutex_lock(&connection_lock);
            active_connections--;
            pthread_mutex_unlock(&connection_lock);
            close(client);
            free(connection);
        }
        pthread_attr_destroy(&attributes);
    }
}

const char *web_proxy_socket_path(const web_proxy_t *proxy) {
    return proxy ? proxy->address.sun_path : NULL;
}

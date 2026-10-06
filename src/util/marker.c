#include "util/marker.h"

#include <string.h>

#define MARKER       "velo-emu-ce-2.0\nversion="
#define MARKER_BYTES 4096

bool marker_value(const uint8_t *image, size_t size, const char *key, char *value, size_t value_size) {
    size_t marker_length = strlen(MARKER), key_length = strlen(key);
    for (size_t at = 0; at + marker_length <= size; at++) {
        const uint8_t *found = memchr(image + at, 'v', size - at);
        if (!found) return false;
        at = (size_t)(found - image);
        if (at + marker_length > size || memcmp(found, MARKER, marker_length)) continue;
        size_t end = at + MARKER_BYTES < size ? at + MARKER_BYTES : size;
        for (size_t line = at; line + key_length + 2 <= end; line++) {
            if (image[line] == 0) break;
            if (image[line] != '\n' || memcmp(image + line + 1, key, key_length) || image[line + 1 + key_length] != '=') continue;
            size_t start = line + key_length + 2, length = 0;
            while (start + length < end && image[start + length] != '\n' && image[start + length] != 0) length++;
            if (length >= value_size) return false;
            memcpy(value, image + start, length);
            value[length] = 0;
            return true;
        }
        return false;
    }
    return false;
}

bool marker_has_set(const char *sets, const char *name) {
    size_t length = strlen(name);
    for (const char *at = sets; *at;) {
        const char *comma = strchr(at, ',');
        size_t item = comma ? (size_t)(comma - at) : strlen(at);
        if (item == length && !memcmp(at, name, length)) return true;
        if (!comma) break;
        at = comma + 1;
    }
    return false;
}

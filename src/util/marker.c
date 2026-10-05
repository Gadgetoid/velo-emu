#include "util/marker.h"

#include <string.h>

#define MARKER       "velo-emu-ce-2.0\nversion="
#define SETS_KEY     "\npatch_sets="
#define MARKER_BYTES 4096

bool marker_patch_sets(const uint8_t *image, size_t size, char *sets, size_t sets_size) {
    size_t marker_length = strlen(MARKER), key_length = strlen(SETS_KEY);
    for (size_t at = 0; at + marker_length <= size; at++) {
        const uint8_t *found = memchr(image + at, 'v', size - at);
        if (!found) return false;
        at = (size_t)(found - image);
        if (at + marker_length > size || memcmp(found, MARKER, marker_length)) continue;
        size_t end = at + MARKER_BYTES < size ? at + MARKER_BYTES : size;
        for (size_t line = at; line + key_length <= end; line++) {
            if (image[line] == 0) break;
            if (memcmp(image + line, SETS_KEY, key_length)) continue;
            size_t start = line + key_length, length = 0;
            while (start + length < end && image[start + length] != '\n' && image[start + length] != 0) length++;
            if (length >= sets_size) return false;
            memcpy(sets, image + start, length);
            sets[length] = 0;
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

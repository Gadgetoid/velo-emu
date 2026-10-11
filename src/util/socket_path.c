#include "util/socket_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool socket_path(char *path, size_t size, const char *name) {
    const char *directory = getenv("TMPDIR");
    if (!directory || !*directory) directory = "/tmp";
    const char *separator = directory[strlen(directory) - 1] == '/' ? "" : "/";
    int length = snprintf(path, size, "%s%s%s-%d.sock", directory, separator, name, (int)getpid());
    if (length >= 0 && (size_t)length < size) return true;
    length = snprintf(path, size, "/tmp/%s-%d.sock", name, (int)getpid());
    return length >= 0 && (size_t)length < size;
}

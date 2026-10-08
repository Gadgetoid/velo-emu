#pragma once

#include <stdint.h>

#define NOTICE_TEXT_CAPACITY 1200
#define NOTICE_SHORT  2
#define NOTICE_MEDIUM 4
#define NOTICE_LONG   6

typedef struct {
    char text[NOTICE_TEXT_CAPACITY];
    uint64_t until;
} notice_t;

void        notice_show(notice_t *notice, const char *text, unsigned seconds);
const char *notice_current(const notice_t *notice);

#pragma once

#include <stddef.h>
#include <stdint.h>

#define NOTICE_QUEUE_CAPACITY 16
#define NOTICE_TEXT_CAPACITY 1200

typedef struct {
    char text[NOTICE_TEXT_CAPACITY];
    uint64_t queued_at;
} notice_entry_t;

typedef struct {
    notice_entry_t entries[NOTICE_QUEUE_CAPACITY];
    size_t head;
    size_t count;
} notice_queue_t;

void        notice_queue_init(notice_queue_t *queue);
void        notice_queue_push(notice_queue_t *queue, const char *text);
const char *notice_queue_current(notice_queue_t *queue);
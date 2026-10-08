#include "app/notices.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define NOTICE_DURATION_NS (2ull * SDL_NS_PER_SECOND)

void notice_queue_init(notice_queue_t *queue) {
    memset(queue, 0, sizeof *queue);
}

void notice_queue_push(notice_queue_t *queue, const char *text) {
    if (!text || !text[0]) return;
    if (queue->count == NOTICE_QUEUE_CAPACITY) {
        queue->head = (queue->head + 1) % NOTICE_QUEUE_CAPACITY;
        queue->count--;
    }
    size_t tail = (queue->head + queue->count) % NOTICE_QUEUE_CAPACITY;
    notice_entry_t *entry = &queue->entries[tail];
    snprintf(entry->text, sizeof entry->text, "%s", text);
    entry->queued_at = SDL_GetTicksNS();
    queue->count++;
}

const char *notice_queue_current(notice_queue_t *queue) {
    uint64_t now = SDL_GetTicksNS();
    while (queue->count) {
        notice_entry_t *entry = &queue->entries[queue->head];
        if (now - entry->queued_at < NOTICE_DURATION_NS) return entry->text;
        queue->head = (queue->head + 1) % NOTICE_QUEUE_CAPACITY;
        queue->count--;
    }
    return NULL;
}
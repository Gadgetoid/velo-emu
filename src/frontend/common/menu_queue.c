#include "frontend/common/menu_queue.h"

#include <string.h>

#include "frontend/common/menu.h"

#define MENU_QUEUE 32

static int queue[MENU_QUEUE];
static int queued;

void menu_queue_push(int item) {
    if (queued < MENU_QUEUE) queue[queued++] = item;
}

int menu_poll(void) {
    if (!queued) return -1;
    int item = queue[0];
    memmove(queue, queue + 1, (size_t)--queued * sizeof queue[0]);
    return item;
}

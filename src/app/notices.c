#include "app/notices.h"

#include <SDL3/SDL.h>

#include <stdio.h>

void notice_show(notice_t *notice, const char *text, unsigned seconds) {
    if (!text || !text[0]) return;
    snprintf(notice->text, sizeof notice->text, "%s", text);
    notice->until = SDL_GetTicksNS() + seconds * SDL_NS_PER_SECOND;
}

const char *notice_current(const notice_t *notice) {
    return notice->until && SDL_GetTicksNS() < notice->until ? notice->text : NULL;
}

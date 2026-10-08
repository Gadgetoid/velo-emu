#include "frontend/android/toast.h"

#include <math.h>
#include <string.h>

#include "frontend/android/android.h"
#include "frontend/android/list.h"
#include "frontend/android/text.h"

#define TOAST_SIZE       0.32f
#define TOAST_PAD_POINTS 12.0f
#define TOAST_GAP_POINTS 16.0f
#define TOAST_WIDTH      0.8f

static char toast[256];

bool android_toast(const char *text) {
    char wanted[sizeof toast];
    SDL_strlcpy(wanted, text ? text : "", sizeof wanted);
    if (wanted[0] >= 'a' && wanted[0] <= 'z') wanted[0] = (char)(wanted[0] - 'a' + 'A');
    if (!strcmp(toast, wanted)) return false;
    memcpy(toast, wanted, sizeof toast);
    return true;
}

void toast_draw(SDL_Window *window, SDL_Renderer *renderer) {
    if (!toast[0]) return;
    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(window, &safe);
    float scale = text_display_scale(window);
    float pad = floorf(TOAST_PAD_POINTS * scale);
    float size = floorf(LIST_ROW_POINTS * scale * TOAST_SIZE);
    float text_w = text_width(renderer, toast, size), limit = width * TOAST_WIDTH - 2 * pad;
    if (text_w > limit) {
        size = floorf(size * limit / text_w);
        text_w = text_width(renderer, toast, size);
    }
    SDL_FRect box = { floorf((width - text_w) / 2 - pad), 0, floorf(text_w + 2 * pad), floorf(size + 2 * pad) };
    box.y = (float)(safe.y + safe.h) - box.h - floorf(TOAST_GAP_POINTS * scale);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ box.x - 1, box.y - 1, box.w + 2, box.h + 2 });
    SDL_SetRenderDrawColor(renderer, 0x18, 0x18, 0x18, 0xF0);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
    text_draw(renderer, box.x + pad, box.y + pad, toast, size);
}

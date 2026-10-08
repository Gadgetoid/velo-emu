#include "app/view.h"
#include "core/lcd.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct view {
    SDL_Window    *window;
    SDL_Renderer  *renderer;
    SDL_Texture   *texture;
    view_display_t display;
    int texture_w, texture_h;
    int output_w, output_h;
    float left, top, right, bottom;
    bool laid_out;
    SDL_FRect dest;
    uint32_t sharp[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];
};

void view_source_size(view_display_t display, int *width, int *height) {
    *width = lcd_width() + (display == VIEW_SIMULATED ? 2 * LCD_MARGIN_X : 0);
    *height = lcd_height() + (display == VIEW_SIMULATED ? 2 * LCD_MARGIN_Y : 0);
}

view_t *view_create(SDL_Window *window, SDL_Renderer *renderer, view_display_t display, int top) {
    view_t *view = calloc(1, sizeof *view);
    view->window = window;
    view->renderer = renderer;
    view->display = display;
    view->top = (float)top;
    return view;
}

void view_destroy(view_t *view) {
    if (!view) return;
    if (view->texture) SDL_DestroyTexture(view->texture);
    free(view);
}

void view_set_display(view_t *view, view_display_t display) {
    if (view->display == display) return;
    view->display = display;
    view->laid_out = false;
}

void view_set_insets(view_t *view, int left, int top, int right, int bottom) {
    if (view->left == (float)left && view->top == (float)top && view->right == (float)right && view->bottom == (float)bottom) return;
    view->left = (float)left;
    view->top = (float)top;
    view->right = (float)right;
    view->bottom = (float)bottom;
    view->laid_out = false;
}

view_display_t view_display(const view_t *view) {
    return view->display;
}

void view_set_screen_size(view_t *view, int width, int height) {
    if (width == lcd_width() && height == lcd_height()) return;
    lcd_set_size(width, height);
    memset(view->sharp, 0, sizeof view->sharp);
    view->laid_out = false;
}

static bool layout(view_t *view) {
    int output_w, output_h;
    SDL_GetRenderOutputSize(view->renderer, &output_w, &output_h);
    if (view->laid_out && output_w == view->output_w && output_h == view->output_h) return false;
    view->output_w = output_w;
    view->output_h = output_h;
    view->laid_out = true;

    int window_w, window_h;
    SDL_GetWindowSize(view->window, &window_w, &window_h);
    float ratio = window_h > 0 ? (float)output_h / window_h : 0;
    float left = floorf(view->left * ratio), top = floorf(view->top * ratio);
    float area_w = output_w - left - floorf(view->right * ratio);
    float area_h = output_h - top - floorf(view->bottom * ratio);
    int source_w, source_h;
    view_source_size(view->display, &source_w, &source_h);
    float fit = fminf(area_w / source_w, area_h / source_h);
    bool whole = fabsf(fit - roundf(fit)) < 0.01f && fit >= 1.0f;
    float scale = whole ? roundf(fit) : fit;

    int texture_w = lcd_width(), texture_h = lcd_height();
    if (view->display == VIEW_SIMULATED) {
        lcd_compose_setup(scale >= 2.0f ? (int)ceilf(scale) : 2);
        lcd_invalidate();
        texture_w = lcd_compose_width();
        texture_h = lcd_compose_height();
    }
    if (!view->texture || texture_w != view->texture_w || texture_h != view->texture_h) {
        if (view->texture) SDL_DestroyTexture(view->texture);
        view->texture = SDL_CreateTexture(view->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, texture_w, texture_h);
        view->texture_w = texture_w;
        view->texture_h = texture_h;
        if (view->display == VIEW_SIMULATED) lcd_invalidate();
    }
    SDL_SetTextureScaleMode(view->texture, whole && (view->display == VIEW_SHARP || (int)scale == lcd_compose_width() / source_w) ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
    view->dest.w = source_w * scale;
    view->dest.h = source_h * scale;
    view->dest.x = left + floorf((area_w - view->dest.w) / 2);
#ifdef __ANDROID__
    if (output_h > output_w) view->dest.y = top;
    else
#endif
    view->dest.y = top + floorf((area_h - view->dest.h) / 2);
    return true;
}

static bool fill_sharp(view_t *view, bool powered) {
    bool changed = false;
    for (int i = 0; i < lcd_width() * lcd_height(); i++) {
        uint32_t grey = powered ? 255u - lcd_framebuffer[i] * 17u : 255u;
        uint32_t pixel = grey | grey << 8 | grey << 16 | 0xff000000u;
        if (view->sharp[i] != pixel) {
            view->sharp[i] = pixel;
            changed = true;
        }
    }
    return changed;
}

bool view_update(view_t *view, float seconds, bool powered) {
    bool changed = layout(view);
    if (view->display == VIEW_SIMULATED) {
        if (lcd_compose(seconds)) {
            SDL_Rect dirty;
            lcd_compose_dirty(&dirty.x, &dirty.y, &dirty.w, &dirty.h);
            int stride = lcd_compose_width();
            SDL_UpdateTexture(view->texture, &dirty, lcd_compose_pixels() + (size_t)dirty.y * stride + dirty.x, stride * 4);
            changed = true;
        }
    } else if (fill_sharp(view, powered) || changed) {
        SDL_UpdateTexture(view->texture, NULL, view->sharp, lcd_width() * 4);
        changed = true;
    }
    return changed;
}

void view_render(view_t *view) {
    SDL_SetRenderDrawColor(view->renderer, 0, 0, 0, 255);
    SDL_RenderClear(view->renderer);
    SDL_RenderTexture(view->renderer, view->texture, NULL, &view->dest);
}

bool view_screen_position(view_t *view, float window_x, float window_y, int *x, int *y) {
    int window_w, window_h;
    SDL_GetWindowSize(view->window, &window_w, &window_h);
    if (!window_w || !window_h || view->dest.w <= 0 || view->dest.h <= 0) return false;
    float pixel_x = window_x * view->output_w / window_w, pixel_y = window_y * view->output_h / window_h;
    int source_w, source_h;
    view_source_size(view->display, &source_w, &source_h);
    int margin_x = view->display == VIEW_SIMULATED ? LCD_MARGIN_X : 0;
    int margin_y = view->display == VIEW_SIMULATED ? LCD_MARGIN_Y : 0;
    float lcd_x = (pixel_x - view->dest.x) / view->dest.w * source_w - margin_x;
    float lcd_y = (pixel_y - view->dest.y) / view->dest.h * source_h - margin_y;
    *x = (int)lcd_x;
    *y = (int)lcd_y;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x >= lcd_width()) *x = lcd_width() - 1;
    if (*y >= lcd_height()) *y = lcd_height() - 1;
    return lcd_x >= 0 && lcd_y >= 0 && lcd_x < lcd_width() && lcd_y < lcd_height();
}

const uint32_t *view_image(view_t *view, int *width, int *height) {
    if (view->display == VIEW_SIMULATED) {
        *width = lcd_compose_width();
        *height = lcd_compose_height();
        return lcd_compose_pixels();
    }
    *width = lcd_width();
    *height = lcd_height();
    return view->sharp;
}

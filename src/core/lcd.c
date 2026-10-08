#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core/lcd.h"

#define GRID_MAX ((SCREEN_MAX_WIDTH + 2 * LCD_MARGIN_X) * (SCREEN_MAX_HEIGHT + 2 * LCD_MARGIN_Y))

uint8_t lcd_framebuffer[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];

static int screen_w = SCREEN_STOCK_WIDTH, screen_h = SCREEN_STOCK_HEIGHT;
static int grid_w = SCREEN_STOCK_WIDTH + 2 * LCD_MARGIN_X, grid_h = SCREEN_STOCK_HEIGHT + 2 * LCD_MARGIN_Y;

static inline int min_int(int a, int b) {
    return a < b ? a : b;
}
static inline int max_int(int a, int b) {
    return a > b ? a : b;
}

typedef struct { float r, g, b; } colour_t;

typedef struct {
    colour_t glass;
    colour_t ink;
    float contrast;
    float shadow;
    float soft_shadow;
    float bloom;
} panel_t;

static const panel_t panel_lit = {
    .glass = { 56, 182, 151 },
    .ink = { 10, 42, 40 },
    .contrast = 0.92f,
    .shadow = 0.22f,
    .soft_shadow = 0.20f,
    .bloom = 0.30f,
};

static const panel_t panel_unlit = {
    .glass = { 148, 160, 126 },
    .ink = { 30, 38, 32 },
    .contrast = 0.90f,
    .shadow = 0.38f,
    .soft_shadow = 0.30f,
    .bloom = 0.0f,
};

static float shown[SCREEN_MAX_WIDTH * SCREEN_MAX_HEIGHT];
static float ink_grid[GRID_MAX];
static float glow_grid[GRID_MAX];
static float glow_scratch[GRID_MAX];
static float soft_grid[GRID_MAX];
static uint32_t *output = NULL;
static float *vignette_x = NULL, *vignette_y = NULL;
static float *grain = NULL;
static int cell = 0, output_w = 0, output_h = 0;

#define DIRTY_MARGIN 6

typedef struct {
    int grid, shadow_grid;
    bool electrode, shadow_electrode, in_panel;
    int soft_x0, soft_x1, glow_x0, glow_x1;
    float soft_fx, glow_fx;
} column_t;

static column_t *columns = NULL;
static int changed_left, changed_right, changed_top, changed_bottom;
static int dirty_x, dirty_y, dirty_w, dirty_h;
static bool force_compose = true;
static bool backlight = true;
static float unlit_level = 1.0f;
static bool powered = true;

void lcd_set_size(int width, int height) {
    if (width == screen_w && height == screen_h) return;
    if (width < 1 || height < 1 || width > SCREEN_MAX_WIDTH || height > SCREEN_MAX_HEIGHT) return;
    screen_w = width;
    screen_h = height;
    grid_w = width + 2 * LCD_MARGIN_X;
    grid_h = height + 2 * LCD_MARGIN_Y;
    memset(shown, 0, sizeof shown);
    free(output);
    output = NULL;
    cell = output_w = output_h = 0;
    force_compose = true;
}

int lcd_width(void) {
    return screen_w;
}
int lcd_height(void) {
    return screen_h;
}

void lcd_set_power(bool on) {
    if (on == powered) return;
    powered = on;
    force_compose = true;
}
static int contrast_level = 5;

void lcd_set_contrast(int level) {
    contrast_level = level < 0 ? 0 : level > 10 ? 10 : level;
    force_compose = true;
}

int lcd_get_contrast(void) {
    return contrast_level;
}

void lcd_set_backlight(bool on) {
    if (on == backlight) return;
    backlight = on;
    force_compose = true;
}
bool lcd_get_backlight(void) {
    return backlight;
}

void lcd_set_unlit_level(float level) {
    if (level == unlit_level) return;
    unlit_level = level;
    lcd_invalidate();
}
uint32_t *lcd_compose_pixels(void) {
    return output;
}
int lcd_compose_width(void) {
    return output_w;
}
int lcd_compose_height(void) {
    return output_h;
}

#define BEZEL_TOP    0.42f
#define BEZEL_LEFT   0.30f
#define BEZEL_RIGHT  0.10f
#define BEZEL_BOTTOM 0.08f
#define GRAIN_FINE   0.11f
#define GRAIN_COARSE 0.08f

static float hash_noise(int x, int y) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u;
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return (h & 0xffff) / 65535.0f;
}

static float value_noise(float x, float y, int seed) {
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float tx = x - ix, ty = y - iy;
    tx = tx * tx * (3 - 2 * tx);
    ty = ty * ty * (3 - 2 * ty);
    int ox = ix + seed * 1013, oy = iy + seed * 7919;
    float top = hash_noise(ox, oy) * (1 - tx) + hash_noise(ox + 1, oy) * tx;
    float bottom = hash_noise(ox, oy + 1) * (1 - tx) + hash_noise(ox + 1, oy + 1) * tx;
    return top * (1 - ty) + bottom * ty;
}

void lcd_compose_setup(int new_cell) {
    if (new_cell < 2) new_cell = 2;
    if (new_cell == cell) return;
    cell = new_cell;
    output_w = grid_w * cell;
    output_h = grid_h * cell;
    free(output);
    free(columns);
    free(vignette_x);
    free(vignette_y);
    free(grain);
    output = malloc((size_t)output_w * output_h * sizeof(uint32_t));
    columns = malloc((size_t)output_w * sizeof *columns);
    int gap = cell >= 4 ? max_int(1, cell / 7) : 1;
    int shadow_offset = max_int(1, cell / 3);
    float soft_offset = 0.75f;
    for (int x = 0; x < output_w; x++) {
        column_t *column = &columns[x];
        int sub_x = x % cell;
        column->grid = x / cell;
        column->in_panel = column->grid >= LCD_MARGIN_X && column->grid < LCD_MARGIN_X + screen_w;
        column->electrode = sub_x < cell - gap;
        int shadow_x = x - shadow_offset;
        column->shadow_grid = shadow_x >= 0 ? shadow_x / cell : -1;
        column->shadow_electrode = shadow_x >= 0 && shadow_x % cell < cell - gap;
        float soft_u = (x + 0.5f) / cell - 0.5f - soft_offset;
        column->soft_x0 = max_int(0, min_int(grid_w - 1, (int)floorf(soft_u)));
        column->soft_x1 = min_int(grid_w - 1, column->soft_x0 + 1);
        column->soft_fx = fminf(1.0f, fmaxf(0.0f, soft_u - column->soft_x0));
        float glow_u = (x + 0.5f) / cell - 0.5f;
        column->glow_x0 = max_int(0, min_int(grid_w - 1, (int)floorf(glow_u)));
        column->glow_x1 = min_int(grid_w - 1, column->glow_x0 + 1);
        column->glow_fx = fminf(1.0f, fmaxf(0.0f, glow_u - column->glow_x0));
    }
    vignette_x = malloc((size_t)output_w * sizeof(float));
    vignette_y = malloc((size_t)output_h * sizeof(float));
    for (int x = 0; x < output_w; x++) {
        float u = (x + 0.5f) / output_w * 2.0f - 1.0f;
        float from_left = (float)x / cell, from_right = (float)(output_w - 1 - x) / cell;
        vignette_x[x] = (1.0f - 0.07f * u * u * u * u)
                        * (1.0f - BEZEL_LEFT * expf(-from_left / 1.1f))
                        * (1.0f - BEZEL_RIGHT * expf(-from_right / 0.7f));
    }
    for (int y = 0; y < output_h; y++) {
        float v = (y + 0.5f) / output_h * 2.0f - 1.0f;
        float from_top = (float)y / cell, from_bottom = (float)(output_h - 1 - y) / cell;
        vignette_y[y] = (1.0f - 0.10f * v * v)
                        * (1.0f - BEZEL_TOP * expf(-from_top / 1.5f))
                        * (1.0f - BEZEL_BOTTOM * expf(-from_bottom / 0.7f));
    }
    grain = malloc((size_t)output_w * output_h * sizeof(float));
    for (int y = 0; y < output_h; y++) {
        for (int x = 0; x < output_w; x++) {
            float fine = hash_noise(x, y) - 0.5f;
            float mottle = value_noise(x / (output_w * 0.32f), y / (output_w * 0.32f), 11) * 0.65f
                           + value_noise(x / (output_w * 0.13f), y / (output_w * 0.13f), 23) * 0.35f;
            grain[(size_t)y * output_w + x] = 1.0f + fine * GRAIN_FINE + (mottle - 0.5f) * GRAIN_COARSE;
        }
    }
    force_compose = true;
}

static void box_blur(float *grid, float *scratch, int radius) {
    float norm = 1.0f / (2 * radius + 1);
    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            float sum = 0;
            for (int k = -radius; k <= radius; k++) {
                int sx = min_int(grid_w - 1, max_int(0, x + k));
                sum += grid[y * grid_w + sx];
            }
            scratch[y * grid_w + x] = sum * norm;
        }
    }
    for (int y = 0; y < grid_h; y++) {
        for (int x = 0; x < grid_w; x++) {
            float sum = 0;
            for (int k = -radius; k <= radius; k++) {
                int sy = min_int(grid_h - 1, max_int(0, y + k));
                sum += scratch[sy * grid_w + x];
            }
            grid[y * grid_w + x] = sum * norm;
        }
    }
}

static float response_scale = 1.0f;

void lcd_set_response(float scale) {
    response_scale = scale;
}

static float pixel_target(int i) {
    return powered ? lcd_framebuffer[i] / 15.0f : 0.0f;
}

void lcd_invalidate(void) {
    force_compose = true;
}

bool lcd_needs_compose(void) {
    if (force_compose) return true;
    for (int i = 0; i < screen_w * screen_h; i++) {
        if (pixel_target(i) != shown[i]) return true;
    }
    return false;
}

static bool settle_pixels(float seconds) {
    float darken = response_scale > 0 ? 1.0f - expf(-seconds / (0.0209f * response_scale)) : 1.0f;
    float lighten = response_scale > 0 ? 1.0f - expf(-seconds / (0.0387f * response_scale)) : 1.0f;
    changed_left = screen_w;
    changed_right = -1;
    changed_top = screen_h;
    changed_bottom = -1;
    for (int y = 0; y < screen_h; y++) {
        for (int x = 0; x < screen_w; x++) {
            int i = y * screen_w + x;
            float target = pixel_target(i);
            float delta = target - shown[i];
            if (delta == 0.0f) continue;
            if (x < changed_left) changed_left = x;
            if (x > changed_right) changed_right = x;
            if (y < changed_top) changed_top = y;
            changed_bottom = y;
            if (fabsf(delta) < 0.01f) shown[i] = target;
            else shown[i] += delta * (delta > 0 ? darken : lighten);
        }
    }
    return changed_right >= 0;
}

void lcd_compose_dirty(int *x, int *y, int *width, int *height) {
    *x = dirty_x;
    *y = dirty_y;
    *width = dirty_w;
    *height = dirty_h;
}

static inline uint8_t to_byte(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return (uint8_t)(value + 0.5f);
}

bool lcd_compose(float seconds) {
    if (!output) return false;
    bool settled_change = settle_pixels(seconds);
    bool full = force_compose;
    force_compose = false;
    if (!settled_change && !full) return false;
    int left = 0, top = 0, right = grid_w - 1, bottom = grid_h - 1;
    if (!full) {
        left = max_int(0, changed_left + LCD_MARGIN_X - DIRTY_MARGIN);
        right = min_int(grid_w - 1, changed_right + LCD_MARGIN_X + DIRTY_MARGIN);
        top = max_int(0, changed_top + LCD_MARGIN_Y - DIRTY_MARGIN);
        bottom = min_int(grid_h - 1, changed_bottom + LCD_MARGIN_Y + DIRTY_MARGIN);
    }
    dirty_x = left * cell;
    dirty_y = top * cell;
    dirty_w = (right - left + 1) * cell;
    dirty_h = (bottom - top + 1) * cell;

    const panel_t *panel = backlight && powered ? &panel_lit : &panel_unlit;
    float level = panel == &panel_unlit ? unlit_level : 1.0f;
    int gap = cell >= 4 ? max_int(1, cell / 7) : 1;
    float gain = (0.55f + contrast_level * 0.06f) / 0.85f;
    float off_bias = contrast_level > 6 ? (contrast_level - 6) * 0.035f : 0.0f;
    int shadow_offset = max_int(1, cell / 3);
    float soft_offset = 0.75f;

    memset(ink_grid, 0, (size_t)grid_w * grid_h * sizeof ink_grid[0]);
    for (int y = 0; y < screen_h; y++) {
        for (int x = 0; x < screen_w; x++) {
            ink_grid[(y + LCD_MARGIN_Y) * grid_w + x + LCD_MARGIN_X] = shown[y * screen_w + x];
        }
    }

    if (panel->bloom > 0) {
        for (int i = 0; i < grid_w * grid_h; i++) glow_grid[i] = 1.0f - fminf(1.0f, ink_grid[i] * panel->contrast * gain);
        box_blur(glow_grid, glow_scratch, 2);
        box_blur(glow_grid, glow_scratch, 2);
    }
    memcpy(soft_grid, ink_grid, (size_t)grid_w * grid_h * sizeof soft_grid[0]);
    box_blur(soft_grid, glow_scratch, 1);

    for (int y = dirty_y; y < dirty_y + dirty_h; y++) {
        int grid_y = y / cell, sub_y = y % cell;
        int shadow_y = y - shadow_offset;
        int shadow_grid_y = shadow_y >= 0 ? shadow_y / cell : -1;
        int shadow_sub_y = shadow_y >= 0 ? shadow_y % cell : 0;
        bool row_in_panel = grid_y >= LCD_MARGIN_Y && grid_y < LCD_MARGIN_Y + screen_h;
        bool row_electrode = sub_y < cell - gap;
        bool row_shadow = shadow_grid_y >= 0 && shadow_sub_y < cell - gap;

        float soft_v = (y + 0.5f) / cell - 0.5f - soft_offset;
        int soft_y0 = max_int(0, min_int(grid_h - 1, (int)floorf(soft_v)));
        int soft_y1 = min_int(grid_h - 1, soft_y0 + 1);
        float soft_fy = fminf(1.0f, fmaxf(0.0f, soft_v - soft_y0));

        float glow_v = (y + 0.5f) / cell - 0.5f;
        int glow_y0 = max_int(0, min_int(grid_h - 1, (int)floorf(glow_v)));
        int glow_y1 = min_int(grid_h - 1, glow_y0 + 1);
        float glow_fy = fminf(1.0f, fmaxf(0.0f, glow_v - glow_y0));

        const float *ink_row = &ink_grid[grid_y * grid_w];
        const float *shadow_row = row_shadow ? &ink_grid[shadow_grid_y * grid_w] : NULL;
        const float *soft_top_row = &soft_grid[soft_y0 * grid_w], *soft_bottom_row = &soft_grid[soft_y1 * grid_w];
        const float *glow_top_row = &glow_grid[glow_y0 * grid_w], *glow_bottom_row = &glow_grid[glow_y1 * grid_w];
        const float *grain_row = &grain[(size_t)y * output_w];
        float vignette_row = vignette_y[y];
        uint32_t *out_row = &output[(size_t)y * output_w];
        for (int x = dirty_x; x < dirty_x + dirty_w; x++) {
            const column_t *column = &columns[x];
            bool electrode = row_in_panel && column->in_panel && row_electrode && column->electrode;
            float ink = electrode ? ink_row[column->grid] : 0.0f;
            float shadow = shadow_row && column->shadow_electrode ? shadow_row[column->shadow_grid] : 0.0f;

            float light = vignette_x[x] * vignette_row * grain_row[x];
            if (electrode) light *= 0.965f;
            light *= 1.0f - shadow * panel->shadow;

            float soft_fx = column->soft_fx;
            float soft_top = soft_top_row[column->soft_x0] * (1 - soft_fx) + soft_top_row[column->soft_x1] * soft_fx;
            float soft_bottom = soft_bottom_row[column->soft_x0] * (1 - soft_fx) + soft_bottom_row[column->soft_x1] * soft_fx;
            light *= 1.0f - (soft_top * (1 - soft_fy) + soft_bottom * soft_fy) * panel->soft_shadow;

            float coverage = fminf(1.0f, ink * panel->contrast * gain + (electrode ? off_bias : 0.0f));
            float r = panel->glass.r * light * (1.0f - coverage) + panel->ink.r * coverage;
            float g = panel->glass.g * light * (1.0f - coverage) + panel->ink.g * coverage;
            float b = panel->glass.b * light * (1.0f - coverage) + panel->ink.b * coverage;

            if (panel->bloom > 0) {
                float glow_fx = column->glow_fx;
                float top_glow = glow_top_row[column->glow_x0] * (1 - glow_fx) + glow_top_row[column->glow_x1] * glow_fx;
                float bottom_glow = glow_bottom_row[column->glow_x0] * (1 - glow_fx) + glow_bottom_row[column->glow_x1] * glow_fx;
                float glow = (top_glow * (1 - glow_fy) + bottom_glow * glow_fy) * panel->bloom * (0.35f + coverage);
                r += panel->glass.r * glow * 0.5f;
                g += panel->glass.g * glow * 0.5f;
                b += panel->glass.b * glow * 0.5f;
            }

            r *= level;
            g *= level;
            b *= level;
            out_row[x] = (uint32_t)to_byte(r) | (uint32_t)to_byte(g) << 8 | (uint32_t)to_byte(b) << 16 | 0xff000000u;
        }
    }
    return true;
}

#include "frontend/android/text.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/stb_truetype.h"

#define LABEL_WIDTH     0.85f
#define FONT_SIZES      6
#define ATLAS_WIDTH     1024
#define ATLAS_HEIGHT    1024
#define FIRST_CHARACTER 32
#define CHARACTER_COUNT 95
#define ELLIPSIS_CODE   0x2026
#define ASCII_MAX       192

typedef struct {
    float size;
    float ascent;
    stbtt_packedchar characters[CHARACTER_COUNT];
    stbtt_packedchar ellipsis;
    SDL_Texture     *texture;
    uint64_t used;
} font_size_t;

static struct {
    unsigned char *data;
    stbtt_fontinfo info;
    bool tried, loaded;
    font_size_t sizes[FONT_SIZES];
    uint64_t clock;
} font;

static const char *FONT_PATHS[] = {
    "/system/fonts/RobotoStatic-Regular.ttf",
    "/system/fonts/Roboto-Regular.ttf",
    "/system/fonts/DroidSans.ttf",
};

static void load_font(void) {
    if (font.tried) return;
    font.tried = true;
    for (size_t i = 0; i < sizeof FONT_PATHS / sizeof FONT_PATHS[0] && !font.loaded; i++) {
        size_t length;
        unsigned char *data = SDL_LoadFile(FONT_PATHS[i], &length);
        if (!data) continue;
        int offset = stbtt_GetFontOffsetForIndex(data, 0);
        if (offset >= 0 && stbtt_InitFont(&font.info, data, offset)) {
            font.data = data;
            font.loaded = true;
        } else {
            SDL_free(data);
        }
    }
}

static font_size_t *font_at(SDL_Renderer *renderer, float size) {
    load_font();
    if (!font.loaded) return NULL;
    size = roundf(size);
    font_size_t *oldest = &font.sizes[0];
    for (int i = 0; i < FONT_SIZES; i++) {
        font_size_t *entry = &font.sizes[i];
        if (entry->texture && entry->size == size) {
            entry->used = ++font.clock;
            return entry;
        }
        if (!entry->texture || entry->used < oldest->used) oldest = entry;
    }
    if (!renderer) return NULL;
    unsigned char *atlas = calloc(ATLAS_WIDTH, ATLAS_HEIGHT);
    uint32_t *pixels = malloc((size_t)ATLAS_WIDTH * ATLAS_HEIGHT * 4);
    if (!atlas || !pixels) {
        free(atlas);
        free(pixels);
        return NULL;
    }
    stbtt_pack_context pack;
    stbtt_pack_range ranges[2] = {
        { size, FIRST_CHARACTER, NULL, CHARACTER_COUNT, oldest->characters, 0, 0 },
        { size, ELLIPSIS_CODE, NULL, 1, &oldest->ellipsis, 0, 0 },
    };
    stbtt_PackBegin(&pack, atlas, ATLAS_WIDTH, ATLAS_HEIGHT, 0, 1, NULL);
    stbtt_PackSetOversampling(&pack, 1, 1);
    stbtt_PackFontRanges(&pack, font.data, 0, ranges, 2);
    stbtt_PackEnd(&pack);
    for (int i = 0; i < ATLAS_WIDTH * ATLAS_HEIGHT; i++) pixels[i] = 0x00FFFFFFu | (uint32_t)atlas[i] << 24;
    if (oldest->texture) SDL_DestroyTexture(oldest->texture);
    oldest->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, ATLAS_WIDTH, ATLAS_HEIGHT);
    if (oldest->texture) {
        SDL_UpdateTexture(oldest->texture, NULL, pixels, ATLAS_WIDTH * 4);
        SDL_SetTextureBlendMode(oldest->texture, SDL_BLENDMODE_BLEND);
    }
    free(atlas);
    free(pixels);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &gap);
    oldest->size = size;
    oldest->ascent = ascent * stbtt_ScaleForPixelHeight(&font.info, size);
    oldest->used = ++font.clock;
    return oldest->texture ? oldest : NULL;
}

static uint32_t next_codepoint(const unsigned char **cursor) {
    const unsigned char *at = *cursor;
    uint32_t codepoint = *at++;
    int extra = codepoint >= 0xF0 ? 3 : codepoint >= 0xE0 ? 2 : codepoint >= 0xC0 ? 1 : 0;
    if (extra) codepoint &= 0x3F >> extra;
    for (int i = 0; i < extra && (*at & 0xC0) == 0x80; i++) codepoint = (codepoint << 6) | (*at++ & 0x3F);
    *cursor = at;
    return codepoint;
}

static const stbtt_packedchar *glyph_of(const font_size_t *entry, uint32_t codepoint) {
    if (codepoint >= FIRST_CHARACTER && codepoint < FIRST_CHARACTER + CHARACTER_COUNT) return &entry->characters[codepoint - FIRST_CHARACTER];
    if (codepoint == ELLIPSIS_CODE) return &entry->ellipsis;
    return NULL;
}

static float debug_scale(float size) {
    return fmaxf(1, floorf(size / SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE));
}

float text_width(SDL_Renderer *renderer, const char *text, float size) {
    font_size_t *entry = font_at(renderer, size);
    float width = 0;
    for (const unsigned char *at = (const unsigned char *)text; *at;) {
        uint32_t codepoint = next_codepoint(&at);
        if (!entry) {
            width += (codepoint == ELLIPSIS_CODE ? 3 : 1) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * debug_scale(size);
            continue;
        }
        const stbtt_packedchar *glyph = glyph_of(entry, codepoint);
        if (glyph) width += glyph->xadvance;
    }
    return width;
}

void text_draw(SDL_Renderer *renderer, float x, float y, const char *text, float size) {
    font_size_t *entry = font_at(renderer, size);
    if (!entry) {
        char ascii[ASCII_MAX];
        size_t length = 0;
        for (const unsigned char *at = (const unsigned char *)text; *at && length + 4 < sizeof ascii;) {
            uint32_t codepoint = next_codepoint(&at);
            if (codepoint == ELLIPSIS_CODE) {
                memcpy(ascii + length, "...", 3);
                length += 3;
            } else if (codepoint >= 0x20 && codepoint < 0x7F) {
                ascii[length++] = (char)codepoint;
            }
        }
        ascii[length] = 0;
        float scale = debug_scale(size);
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_RenderDebugText(renderer, floorf(x / scale), floorf((y + (size - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * scale) / 2) / scale), ascii);
        SDL_SetRenderScale(renderer, 1, 1);
        return;
    }
    Uint8 r, g, b, a;
    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_SetTextureColorMod(entry->texture, r, g, b);
    float pen = roundf(x), baseline = roundf(y + entry->ascent);
    for (const unsigned char *at = (const unsigned char *)text; *at;) {
        const stbtt_packedchar *glyph = glyph_of(entry, next_codepoint(&at));
        if (!glyph) continue;
        SDL_FRect source = { glyph->x0, glyph->y0, (float)(glyph->x1 - glyph->x0), (float)(glyph->y1 - glyph->y0) };
        SDL_FRect target = { roundf(pen + glyph->xoff), baseline + glyph->yoff, source.w, source.h };
        SDL_RenderTexture(renderer, entry->texture, &source, &target);
        pen += glyph->xadvance;
    }
}

void text_draw_centred(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float size) {
    float width = text_width(renderer, label, size);
    text_draw(renderer, rect->x + (rect->w - width) / 2, rect->y + (rect->h - size) / 2, label, size);
}

float text_fit(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float height) {
    float size = rect->h * height;
    float width = text_width(renderer, label, size);
    if (width > rect->w * LABEL_WIDTH) size *= rect->w * LABEL_WIDTH / width;
    return floorf(size);
}

float text_display_scale(SDL_Window *window) {
    float scale = window ? SDL_GetWindowDisplayScale(window) : 1.0f;
    return scale > 0 ? scale : 1.0f;
}

SDL_FRect text_inset(SDL_FRect rect, float gap) {
    return (SDL_FRect){ floorf(rect.x + gap / 2), floorf(rect.y + gap / 2), floorf(rect.w - gap), floorf(rect.h - gap) };
}

bool text_contains(const SDL_FRect *rect, float x, float y) {
    return rect->w > 0 && x >= rect->x && x < rect->x + rect->w && y >= rect->y && y < rect->y + rect->h;
}

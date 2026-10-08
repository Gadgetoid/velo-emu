#include "frontend/linux/ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "vendor/stb_truetype.h"

#define FONT_SIZE        13.0f
#define FALLBACK_ADVANCE ((float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE)
#define ATLAS_SIZE       512
#define ASCII_FIRST      32
#define ASCII_COUNT      95

static const palette_t LIGHT = {
    { 246, 245, 244, 255 }, { 222, 221, 218, 255 }, { 213, 208, 204, 255 }, { 46, 52, 54, 255 },
    { 255, 255, 255, 255 }, { 190, 186, 182, 255 }, { 53, 132, 228, 255 }, { 255, 255, 255, 255 },
    { 154, 153, 150, 255 }, { 119, 118, 123, 255 }, { 225, 222, 219, 255 },
};

static const palette_t DARK = {
    { 48, 48, 48, 255 }, { 70, 70, 70, 255 }, { 28, 28, 28, 255 }, { 238, 238, 236, 255 },
    { 56, 56, 56, 255 }, { 24, 24, 24, 255 }, { 53, 132, 228, 255 }, { 255, 255, 255, 255 },
    { 125, 125, 125, 255 }, { 165, 165, 165, 255 }, { 78, 78, 78, 255 },
};

static const int EXTRA_CODEPOINTS[GLYPH_EXTRA_COUNT] = { 0x2026, 0x2713, 0x25b8 };

static const char *FONT_PATHS[] = {
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
};

static float density = 1.0f;

static struct {
    unsigned char   *data;
    stbtt_fontinfo info;
    stbtt_packedchar ascii[ASCII_COUNT];
    stbtt_packedchar extra[GLYPH_EXTRA_COUNT];
    bool has_extra[GLYPH_EXTRA_COUNT];
    unsigned char   *atlas;
    Uint32          *atlas_pixels;
    float baked_density;
    float ascent, line_height;
    int generation;
    bool loaded, baked;
} font;

static bool load_font_file(const char *path) {
    size_t size;
    unsigned char *data = SDL_LoadFile(path, &size);
    if (!data) return false;
    int offset = stbtt_GetFontOffsetForIndex(data, 0);
    if (offset < 0 || !stbtt_InitFont(&font.info, data, offset)) {
        SDL_free(data);
        return false;
    }
    font.data = data;
    return true;
}

static bool load_matched_font(void) {
    FILE *match = popen("fc-match -f '%{file}' sans-serif:style=Regular 2>/dev/null", "r");
    if (!match) return false;
    char path[1024] = "";
    bool read = fgets(path, sizeof path, match) != NULL;
    pclose(match);
    return read && path[0] == '/' && load_font_file(path);
}

void ui_load_font(void) {
    if (font.loaded) return;
    font.loaded = load_matched_font();
    for (size_t i = 0; !font.loaded && i < sizeof FONT_PATHS / sizeof FONT_PATHS[0]; i++) font.loaded = load_font_file(FONT_PATHS[i]);
    if (!font.loaded) return;
    for (int i = 0; i < GLYPH_EXTRA_COUNT; i++) font.has_extra[i] = stbtt_FindGlyphIndex(&font.info, EXTRA_CODEPOINTS[i]) != 0;
}

static void bake_font(void) {
    if (!font.loaded || (font.baked && font.baked_density == density)) return;
    font.baked = false;
    if (!font.atlas) font.atlas = malloc(ATLAS_SIZE * ATLAS_SIZE);
    if (!font.atlas_pixels) font.atlas_pixels = malloc((size_t)ATLAS_SIZE * ATLAS_SIZE * 4);
    if (!font.atlas || !font.atlas_pixels) return;
    float size = FONT_SIZE * density;
    stbtt_pack_context pack;
    stbtt_pack_range ranges[2] = {
        { STBTT_POINT_SIZE(size), ASCII_FIRST, NULL, ASCII_COUNT, font.ascii, 0, 0 },
        { STBTT_POINT_SIZE(size), 0, (int *)EXTRA_CODEPOINTS, GLYPH_EXTRA_COUNT, font.extra, 0, 0 },
    };
    if (!stbtt_PackBegin(&pack, font.atlas, ATLAS_SIZE, ATLAS_SIZE, 0, 1, NULL)) return;
    bool packed = stbtt_PackFontRanges(&pack, font.data, 0, ranges, 2) != 0;
    stbtt_PackEnd(&pack);
    if (!packed) {
        font.loaded = false;
        return;
    }
    for (int i = 0; i < ATLAS_SIZE * ATLAS_SIZE; i++) font.atlas_pixels[i] = 0x00ffffffu | (Uint32)font.atlas[i] << 24;
    float scale = stbtt_ScaleForMappingEmToPixels(&font.info, size);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font.info, &ascent, &descent, &gap);
    font.ascent = ascent * scale / density;
    font.line_height = (ascent - descent) * scale / density;
    font.baked_density = density;
    font.baked = true;
    font.generation++;
}

void ui_prepare(SDL_Window *window) {
    float pixel_density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    density = pixel_density > 0 ? pixel_density : 1.0f;
    bake_font();
}

const palette_t *ui_palette(void) {
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? &DARK : &LIGHT;
}

static bool ensure_glyphs(canvas_t *canvas) {
    if (!font.baked) return false;
    if (canvas->glyphs && canvas->glyph_generation == font.generation) return true;
    if (canvas->glyphs) SDL_DestroyTexture(canvas->glyphs);
    canvas->glyphs = SDL_CreateTexture(canvas->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, ATLAS_SIZE, ATLAS_SIZE);
    if (!canvas->glyphs) return false;
    SDL_UpdateTexture(canvas->glyphs, NULL, font.atlas_pixels, ATLAS_SIZE * 4);
    SDL_SetTextureBlendMode(canvas->glyphs, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(canvas->glyphs, SDL_SCALEMODE_NEAREST);
    canvas->glyph_generation = font.generation;
    return true;
}

static int next_codepoint(const char **text) {
    const unsigned char *at = (const unsigned char *)*text;
    int codepoint = *at++;
    int extra = codepoint >= 0xf0 ? 3 : codepoint >= 0xe0 ? 2 : codepoint >= 0xc0 ? 1 : 0;
    if (extra) codepoint &= 0x3f >> extra;
    for (int i = 0; i < extra && (*at & 0xc0) == 0x80; i++) codepoint = codepoint << 6 | (*at++ & 0x3f);
    *text = (const char *)at;
    return codepoint;
}

static const stbtt_packedchar *glyph_for(int codepoint) {
    if (codepoint >= ASCII_FIRST && codepoint < ASCII_FIRST + ASCII_COUNT) return &font.ascii[codepoint - ASCII_FIRST];
    for (int i = 0; i < GLYPH_EXTRA_COUNT; i++) {
        if (codepoint == EXTRA_CODEPOINTS[i] && font.has_extra[i]) return &font.extra[i];
    }
    return NULL;
}

float ui_text_width(const char *text) {
    float width = 0;
    while (*text) {
        int codepoint = next_codepoint(&text);
        if (!font.baked) {
            width += FALLBACK_ADVANCE;
            continue;
        }
        const stbtt_packedchar *glyph = glyph_for(codepoint);
        if (glyph) width += glyph->xadvance / density;
        else if (codepoint == EXTRA_CODEPOINTS[GLYPH_ELLIPSIS]) width += 3 * font.ascii['.' - ASCII_FIRST].xadvance / density;
    }
    return width;
}

static void set_colour(SDL_Renderer *renderer, colour_t colour) {
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, colour.a);
}

void ui_fill(canvas_t *canvas, float x, float y, float width, float height, colour_t colour) {
    SDL_FRect rect = { floorf(x * density), floorf(y * density), 0, 0 };
    rect.w = floorf((x + width) * density) - rect.x;
    rect.h = floorf((y + height) * density) - rect.y;
    set_colour(canvas->renderer, colour);
    SDL_RenderFillRect(canvas->renderer, &rect);
}

static void draw_glyph(canvas_t *canvas, const stbtt_packedchar *glyph, float *pen_x, float baseline) {
    stbtt_aligned_quad quad;
    float y = 0;
    stbtt_GetPackedQuad(glyph, ATLAS_SIZE, ATLAS_SIZE, 0, pen_x, &y, &quad, 1);
    SDL_FRect source = { quad.s0 * ATLAS_SIZE, quad.t0 * ATLAS_SIZE, (quad.s1 - quad.s0) * ATLAS_SIZE, (quad.t1 - quad.t0) * ATLAS_SIZE };
    SDL_FRect target = { quad.x0, roundf(baseline) + quad.y0, quad.x1 - quad.x0, quad.y1 - quad.y0 };
    SDL_RenderTexture(canvas->renderer, canvas->glyphs, &source, &target);
}

void ui_text(canvas_t *canvas, float x, float top, float height, const char *text, colour_t colour) {
    if (!ensure_glyphs(canvas)) {
        char ascii[128];
        size_t length = 0;
        while (*text && length < sizeof ascii - 1) {
            int codepoint = next_codepoint(&text);
            ascii[length++] = codepoint < 128 ? (char)codepoint : '.';
        }
        ascii[length] = 0;
        SDL_SetRenderScale(canvas->renderer, density, density);
        set_colour(canvas->renderer, colour);
        SDL_RenderDebugText(canvas->renderer, x, top + (height - 8) / 2, ascii);
        SDL_SetRenderScale(canvas->renderer, 1, 1);
        return;
    }
    SDL_SetTextureColorMod(canvas->glyphs, colour.r, colour.g, colour.b);
    float pen_x = roundf(x * density);
    float baseline = (top + (height - font.line_height) / 2 + font.ascent) * density;
    while (*text) {
        int codepoint = next_codepoint(&text);
        const stbtt_packedchar *glyph = glyph_for(codepoint);
        if (glyph) draw_glyph(canvas, glyph, &pen_x, baseline);
        else if (codepoint == EXTRA_CODEPOINTS[GLYPH_ELLIPSIS]) {
            for (int i = 0; i < 3; i++) draw_glyph(canvas, &font.ascii['.' - ASCII_FIRST], &pen_x, baseline);
        }
    }
}

static void draw_polygon(canvas_t *canvas, const float *points, int count, colour_t colour) {
    SDL_Vertex vertices[8];
    int indices[18];
    SDL_FColor fill = { colour.r / 255.0f, colour.g / 255.0f, colour.b / 255.0f, colour.a / 255.0f };
    for (int i = 0; i < count; i++) vertices[i] = (SDL_Vertex){ { points[i * 2] * density, points[i * 2 + 1] * density }, fill, { 0, 0 } };
    int index_count = 0;
    for (int i = 1; i + 1 < count; i++) {
        indices[index_count++] = 0;
        indices[index_count++] = i;
        indices[index_count++] = i + 1;
    }
    SDL_RenderGeometry(canvas->renderer, NULL, vertices, count, indices, index_count);
}

void ui_mark(canvas_t *canvas, glyph_t glyph, float centre_x, float top, float height, colour_t colour) {
    if (font.has_extra[glyph] && ensure_glyphs(canvas)) {
        float width = font.extra[glyph].xadvance / density;
        char text[4] = { 0 };
        int codepoint = EXTRA_CODEPOINTS[glyph];
        text[0] = (char)(0xe0 | codepoint >> 12);
        text[1] = (char)(0x80 | (codepoint >> 6 & 0x3f));
        text[2] = (char)(0x80 | (codepoint & 0x3f));
        ui_text(canvas, centre_x - width / 2, top, height, text, colour);
        return;
    }
    float y = top + height / 2;
    if (glyph == GLYPH_ARROW) {
        float arrow[] = { centre_x - 2, y - 4, centre_x + 3, y, centre_x - 2, y + 4 };
        draw_polygon(canvas, arrow, 3, colour);
    } else {
        float short_stroke[] = { centre_x - 5, y, centre_x - 3.5f, y - 1.5f, centre_x - 1, y + 1, centre_x - 2.5f, y + 2.5f };
        float long_stroke[] = { centre_x - 2.5f, y + 2.5f, centre_x + 4, y - 4, centre_x + 5.5f, y - 2.5f, centre_x - 1, y + 4 };
        draw_polygon(canvas, short_stroke, 4, colour);
        draw_polygon(canvas, long_stroke, 4, colour);
    }
}

bool ui_inside(float x, float y, float left, float top, float width, float height) {
    return x >= left && y >= top && x < left + width && y < top + height;
}

void ui_release_glyphs(canvas_t *canvas) {
    if (canvas->glyphs) SDL_DestroyTexture(canvas->glyphs);
    canvas->glyphs = NULL;
    canvas->glyph_generation = 0;
}

void ui_destroy_canvas(canvas_t *canvas) {
    ui_release_glyphs(canvas);
    if (canvas->renderer) SDL_DestroyRenderer(canvas->renderer);
    if (canvas->window) SDL_DestroyWindow(canvas->window);
    *canvas = (canvas_t){ 0 };
}

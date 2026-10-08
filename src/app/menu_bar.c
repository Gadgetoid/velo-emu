#include "app/dialog.h"
#include "app/menu_layout.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/stb_truetype.h"

#define BAR_HEIGHT       24.0f
#define BAR_INSET        4.0f
#define BAR_PADDING      9.0f
#define ROW_HEIGHT       24.0f
#define SEPARATOR_HEIGHT 9.0f
#define HEADING_HEIGHT   20.0f
#define HEADING_INSET    12.0f
#define MENU_PADDING     4.0f
#define CHECK_COLUMN     26.0f
#define ARROW_COLUMN     22.0f
#define TEXT_RIGHT       14.0f
#define SHORTCUT_GAP     32.0f
#define MENU_MIN_WIDTH   160.0f
#define SUBMENU_OVERLAP  3.0f
#define FONT_SIZE        13.0f
#define FALLBACK_ADVANCE ((float)SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE)

#define MENU_QUEUE  32
#define MAX_MENUS   16
#define MAX_ITEMS   192
#define MENU_ITEMS  48
#define MAX_DEPTH   4
#define ATLAS_SIZE  512
#define ASCII_FIRST 32
#define ASCII_COUNT 95

typedef enum { ITEM_ACTION, ITEM_SEPARATOR, ITEM_SUBMENU, ITEM_HEADING } item_kind_t;

typedef struct {
    item_kind_t kind;
    int tag;
    int submenu;
    char title[96];
    char shortcut[32];
    SDL_Keycode key;
    SDL_Keymod modifiers;
} item_t;

typedef struct {
    char title[32];
    int first, count;
    float bar_x, bar_width;
} menu_t;

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *glyphs;
    int glyph_generation;
} canvas_t;

typedef struct {
    int menu;
    int hover;
    float width, height;
    canvas_t canvas;
} level_t;

typedef struct { Uint8 r, g, b, a; } colour_t;

typedef struct {
    colour_t bar, bar_open, bar_border, text, menu, menu_border, highlight, highlight_text, disabled, shortcut, separator;
} palette_t;

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

enum { GLYPH_ELLIPSIS, GLYPH_CHECK, GLYPH_ARROW, GLYPH_EXTRA_COUNT };
static int EXTRA_CODEPOINTS[GLYPH_EXTRA_COUNT] = { 0x2026, 0x2713, 0x25b8 };

static struct {
    unsigned char   *data;
    stbtt_fontinfo info;
    stbtt_packedchar ascii[ASCII_COUNT];
    stbtt_packedchar extra[GLYPH_EXTRA_COUNT];
    bool has_extra[GLYPH_EXTRA_COUNT];
    unsigned char   *atlas;
    Uint32          *atlas_pixels;
    float density;
    float ascent, line_height;
    int generation;
    bool loaded, baked;
} font;

static SDL_Window *main_window;
static float density = 1.0f;
static canvas_t bar_canvas;
static menu_t menus[MAX_MENUS];
static int menu_count;
static int top_menus[MAX_MENUS];
static int top_count;
static item_t items[MAX_ITEMS];
static int item_count;
static int item_of_tag[MENU_COUNT];
static bool enabled[MENU_COUNT], checked[MENU_COUNT], hidden[MENU_COUNT];
static level_t levels[MAX_DEPTH];
static int depth;
static int open_top = -1;
static int queue[MENU_QUEUE];
static int queued;
static bool popup_failed;

static const char *FONT_PATHS[] = {
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
};

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

static void load_font(void) {
    font.loaded = load_matched_font();
    for (size_t i = 0; !font.loaded && i < sizeof FONT_PATHS / sizeof FONT_PATHS[0]; i++) font.loaded = load_font_file(FONT_PATHS[i]);
    if (!font.loaded) return;
    for (int i = 0; i < GLYPH_EXTRA_COUNT; i++) font.has_extra[i] = stbtt_FindGlyphIndex(&font.info, EXTRA_CODEPOINTS[i]) != 0;
}

static float window_density(void) {
    float pixel_density = main_window ? SDL_GetWindowPixelDensity(main_window) : 1.0f;
    return pixel_density > 0 ? pixel_density : 1.0f;
}

static void bake_font(void) {
    density = window_density();
    if (!font.loaded || (font.baked && font.density == density)) return;
    if (!font.atlas) font.atlas = malloc(ATLAS_SIZE * ATLAS_SIZE);
    if (!font.atlas_pixels) font.atlas_pixels = malloc((size_t)ATLAS_SIZE * ATLAS_SIZE * 4);
    if (!font.atlas || !font.atlas_pixels) return;
    float size = FONT_SIZE * density;
    stbtt_pack_context pack;
    stbtt_pack_range ranges[2] = {
        { STBTT_POINT_SIZE(size), ASCII_FIRST, NULL, ASCII_COUNT, font.ascii, 0, 0 },
        { STBTT_POINT_SIZE(size), 0, EXTRA_CODEPOINTS, GLYPH_EXTRA_COUNT, font.extra, 0, 0 },
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
    font.density = density;
    font.baked = true;
    font.generation++;
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

static float text_width(const char *text) {
    float width = 0;
    while (*text) {
        int codepoint = next_codepoint(&text);
        if (!font.baked) { width += FALLBACK_ADVANCE; continue; }
        const stbtt_packedchar *glyph = glyph_for(codepoint);
        if (glyph) width += glyph->xadvance / font.density;
        else if (codepoint == 0x2026) width += 3 * font.ascii['.' - ASCII_FIRST].xadvance / font.density;
    }
    return width;
}

static void set_colour(SDL_Renderer *renderer, colour_t colour) {
    SDL_SetRenderDrawColor(renderer, colour.r, colour.g, colour.b, colour.a);
}

static void fill_rect(canvas_t *canvas, float x, float y, float width, float height, colour_t colour) {
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

static void draw_text(canvas_t *canvas, float x, float top, float height, const char *text, colour_t colour) {
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
    float pen_x = roundf(x * font.density);
    float baseline = (top + (height - font.line_height) / 2 + font.ascent) * font.density;
    while (*text) {
        int codepoint = next_codepoint(&text);
        const stbtt_packedchar *glyph = glyph_for(codepoint);
        if (glyph) draw_glyph(canvas, glyph, &pen_x, baseline);
        else if (codepoint == 0x2026) {
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

static void draw_mark(canvas_t *canvas, int glyph_index, float centre_x, float top, colour_t colour) {
    if (font.has_extra[glyph_index] && ensure_glyphs(canvas)) {
        float width = font.extra[glyph_index].xadvance / font.density;
        char text[4] = { 0 };
        int codepoint = EXTRA_CODEPOINTS[glyph_index];
        text[0] = (char)(0xe0 | codepoint >> 12);
        text[1] = (char)(0x80 | (codepoint >> 6 & 0x3f));
        text[2] = (char)(0x80 | (codepoint & 0x3f));
        draw_text(canvas, centre_x - width / 2, top, ROW_HEIGHT, text, colour);
        return;
    }
    float y = top + ROW_HEIGHT / 2;
    if (glyph_index == GLYPH_ARROW) {
        float arrow[] = { centre_x - 2, y - 4, centre_x + 3, y, centre_x - 2, y + 4 };
        draw_polygon(canvas, arrow, 3, colour);
    } else {
        float short_stroke[] = { centre_x - 5, y, centre_x - 3.5f, y - 1.5f, centre_x - 1, y + 1, centre_x - 2.5f, y + 2.5f };
        float long_stroke[] = { centre_x - 2.5f, y + 2.5f, centre_x + 4, y - 4, centre_x + 5.5f, y - 2.5f, centre_x - 1, y + 4 };
        draw_polygon(canvas, short_stroke, 4, colour);
        draw_polygon(canvas, long_stroke, 4, colour);
    }
}

static const palette_t *palette(void) {
    return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK ? &DARK : &LIGHT;
}

static void set_shortcut(item_t *item, const menu_entry_t *entry) {
    if (entry->tag == MENU_FULL_SCREEN) {
        item->key = SDLK_F11;
        item->modifiers = SDL_KMOD_NONE;
        snprintf(item->shortcut, sizeof item->shortcut, "F11");
        return;
    }
    if (!entry->key || (entry->modifiers & MENU_KEY_CONTROL)) return;
    bool shift = (entry->modifiers & MENU_KEY_SHIFT) != 0;
    item->key = (SDL_Keycode)(unsigned char)entry->key;
    item->modifiers = SDL_KMOD_CTRL | SDL_KMOD_ALT | (shift ? SDL_KMOD_SHIFT : 0);
    snprintf(item->shortcut, sizeof item->shortcut, "%sCtrl+Alt+%c", shift ? "Shift+" : "", toupper((unsigned char)entry->key));
}

static void skip_menu(int *cursor) {
    for (int nesting = 1; *cursor < MENU_ENTRY_COUNT && nesting > 0; (*cursor)++) {
        menu_entry_kind_t kind = MENU_ENTRIES[*cursor].kind;
        if (kind == MENU_ENTRY_SUBMENU) nesting++;
        else if (kind == MENU_ENTRY_END) nesting--;
    }
}

static int parse_menu(int *cursor, const char *title) {
    if (menu_count >= MAX_MENUS) {
        skip_menu(cursor);
        return -1;
    }
    item_t local[MENU_ITEMS];
    int local_count = 0;
    int index = menu_count++;
    snprintf(menus[index].title, sizeof menus[index].title, "%s", title);
    while (*cursor < MENU_ENTRY_COUNT) {
        const menu_entry_t *entry = &MENU_ENTRIES[(*cursor)++];
        if (entry->kind == MENU_ENTRY_END) break;
        item_t item = { ITEM_ACTION, -1, -1, "", "", 0, SDL_KMOD_NONE };
        if (entry->kind == MENU_ENTRY_SEPARATOR) {
            item.kind = ITEM_SEPARATOR;
        } else if (entry->kind == MENU_ENTRY_HEADING) {
            item.kind = ITEM_HEADING;
            snprintf(item.title, sizeof item.title, "%s", entry->title);
        } else if (entry->kind == MENU_ENTRY_SUBMENU) {
            item.kind = ITEM_SUBMENU;
            snprintf(item.title, sizeof item.title, "%s", entry->title);
            item.submenu = parse_menu(cursor, entry->title);
            if (item.submenu < 0) continue;
        } else if (entry->kind == MENU_ENTRY_ITEM) {
            item.tag = entry->tag;
            snprintf(item.title, sizeof item.title, "%s", entry->title);
            set_shortcut(&item, entry);
        } else {
            continue;
        }
        if (local_count < MENU_ITEMS) local[local_count++] = item;
    }
    menus[index].first = item_count;
    menus[index].count = 0;
    for (int i = 0; i < local_count && item_count < MAX_ITEMS; i++) {
        if (local[i].tag >= 0 && local[i].tag < MENU_COUNT) item_of_tag[local[i].tag] = item_count;
        items[item_count++] = local[i];
        menus[index].count++;
    }
    return index;
}

static bool item_visible(const item_t *item) {
    return item->kind != ITEM_ACTION || !hidden[item->tag];
}

static bool item_selectable(const item_t *item) {
    if (item->kind == ITEM_SEPARATOR || item->kind == ITEM_HEADING || !item_visible(item)) return false;
    return item->kind == ITEM_SUBMENU || enabled[item->tag];
}

static float item_height(const item_t *item) {
    if (item->kind == ITEM_SEPARATOR) return SEPARATOR_HEIGHT;
    return item->kind == ITEM_HEADING ? HEADING_HEIGHT : ROW_HEIGHT;
}

static void measure_menu(int menu, float *width, float *height) {
    float title_width = 0, shortcut_width = 0, rows = 0;
    bool has_submenu = false;
    for (int i = menus[menu].first; i < menus[menu].first + menus[menu].count; i++) {
        const item_t *item = &items[i];
        if (!item_visible(item)) continue;
        rows += item_height(item);
        if (item->kind == ITEM_SEPARATOR) continue;
        title_width = fmaxf(title_width, text_width(item->title));
        if (item->shortcut[0]) shortcut_width = fmaxf(shortcut_width, text_width(item->shortcut));
        if (item->kind == ITEM_SUBMENU) has_submenu = true;
    }
    float total = CHECK_COLUMN + title_width + (shortcut_width > 0 ? SHORTCUT_GAP + shortcut_width : 0) + (has_submenu ? ARROW_COLUMN : TEXT_RIGHT);
    *width = ceilf(fmaxf(total, MENU_MIN_WIDTH));
    *height = ceilf(rows + 2 * MENU_PADDING);
}

static void layout_bar(void) {
    float x = BAR_INSET;
    for (int i = 0; i < top_count; i++) {
        menu_t *menu = &menus[top_menus[i]];
        menu->bar_x = x;
        menu->bar_width = ceilf(text_width(menu->title) + 2 * BAR_PADDING);
        x += menu->bar_width;
    }
}

static void level_origin(int level, float *x, float *y) {
    *x = 0;
    *y = 0;
    for (int i = 0; i <= level && i < depth; i++) {
        int offset_x = 0, offset_y = 0;
        SDL_GetWindowPosition(levels[i].canvas.window, &offset_x, &offset_y);
        *x += (float)offset_x;
        *y += (float)offset_y;
    }
}

static float item_top(const level_t *level, int index) {
    float y = MENU_PADDING;
    for (int i = menus[level->menu].first; i < index; i++) {
        if (item_visible(&items[i])) y += item_height(&items[i]);
    }
    return y;
}

static void destroy_canvas(canvas_t *canvas) {
    if (canvas->glyphs) SDL_DestroyTexture(canvas->glyphs);
    if (canvas->renderer) SDL_DestroyRenderer(canvas->renderer);
    if (canvas->window) SDL_DestroyWindow(canvas->window);
    *canvas = (canvas_t){ 0 };
}

static void close_levels(int from) {
    while (depth > from) destroy_canvas(&levels[--depth].canvas);
    if (depth == 0) open_top = -1;
}

static bool open_level(int level, int menu, float x, float y) {
    close_levels(level);
    float width, height;
    measure_menu(menu, &width, &height);
    float parent_x = 0, parent_y = 0;
    if (level > 0) level_origin(level - 1, &parent_x, &parent_y);
    SDL_Window *parent = level > 0 ? levels[level - 1].canvas.window : main_window;
    SDL_Window *window = SDL_CreatePopupWindow(parent, (int)(x - parent_x), (int)(y - parent_y), (int)width, (int)height,
                                               SDL_WINDOW_POPUP_MENU | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer *renderer = window ? SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER) : NULL;
    if (!renderer) {
        if (!popup_failed) fprintf(stderr, "menu: %s\n", SDL_GetError());
        popup_failed = true;
        if (window) SDL_DestroyWindow(window);
        return false;
    }
    levels[level] = (level_t){ menu, -1, width, height, { window, renderer, NULL, 0 } };
    depth = level + 1;
    return true;
}

static int first_selectable(int menu, int from, int step) {
    int count = menus[menu].count;
    for (int i = 0; i < count; i++) {
        int index = menus[menu].first + ((from - menus[menu].first + step * (i + 1)) % count + count) % count;
        if (item_selectable(&items[index])) return index;
    }
    return -1;
}

static void open_top_menu(int top, bool select_first) {
    close_levels(0);
    layout_bar();
    if (!open_level(0, top_menus[top], menus[top_menus[top]].bar_x, BAR_HEIGHT)) return;
    open_top = top;
    if (select_first) levels[0].hover = first_selectable(levels[0].menu, menus[levels[0].menu].first - 1, 1);
}

static void open_submenu(int level, int index) {
    if (items[index].kind != ITEM_SUBMENU) return;
    if (depth > level + 1 && levels[level + 1].menu == items[index].submenu) return;
    float origin_x, origin_y;
    level_origin(level, &origin_x, &origin_y);
    open_level(level + 1, items[index].submenu, origin_x + levels[level].width - SUBMENU_OVERLAP, origin_y + item_top(&levels[level], index) - MENU_PADDING);
}

static void activate(int index) {
    const item_t *item = &items[index];
    if (!item_selectable(item)) return;
    if (item->kind == ITEM_SUBMENU) {
        for (int level = 0; level < depth; level++) {
            if (levels[level].hover == index) {
                open_submenu(level, index);
                if (depth > level + 1) levels[level + 1].hover = first_selectable(levels[level + 1].menu, menus[levels[level + 1].menu].first - 1, 1);
            }
        }
        return;
    }
    close_levels(0);
    if (queued < MENU_QUEUE) queue[queued++] = item->tag;
}

static int bar_hit(float x, float y) {
    if (y < 0 || y >= BAR_HEIGHT) return -1;
    layout_bar();
    for (int i = 0; i < top_count; i++) {
        const menu_t *menu = &menus[top_menus[i]];
        if (x >= menu->bar_x && x < menu->bar_x + menu->bar_width) return i;
    }
    return -1;
}

static int level_hit(float x, float y, int *index) {
    for (int level = depth - 1; level >= 0; level--) {
        float origin_x, origin_y;
        level_origin(level, &origin_x, &origin_y);
        float local_x = x - origin_x, local_y = y - origin_y;
        if (local_x < 0 || local_y < 0 || local_x >= levels[level].width || local_y >= levels[level].height) continue;
        *index = -1;
        float top = MENU_PADDING;
        const menu_t *menu = &menus[levels[level].menu];
        for (int i = menu->first; i < menu->first + menu->count; i++) {
            if (!item_visible(&items[i])) continue;
            float height = item_height(&items[i]);
            if (local_y >= top && local_y < top + height) *index = i;
            top += height;
        }
        return level;
    }
    return -1;
}

static int level_of_window(SDL_WindowID id) {
    for (int level = 0; level < depth; level++) {
        if (SDL_GetWindowID(levels[level].canvas.window) == id) return level;
    }
    return -1;
}

static bool to_main_point(SDL_WindowID id, float x, float y, float *main_x, float *main_y) {
    if (main_window && id == SDL_GetWindowID(main_window)) {
        *main_x = x;
        *main_y = y;
        return true;
    }
    int level = level_of_window(id);
    if (level < 0) return false;
    float origin_x, origin_y;
    level_origin(level, &origin_x, &origin_y);
    *main_x = x + origin_x;
    *main_y = y + origin_y;
    return true;
}

static void hover_at(float x, float y) {
    int top = bar_hit(x, y);
    if (top >= 0) {
        if (top != open_top) open_top_menu(top, false);
        return;
    }
    int index;
    int level = level_hit(x, y, &index);
    if (level < 0) return;
    levels[level].hover = index >= 0 && item_selectable(&items[index]) ? index : -1;
    if (levels[level].hover >= 0 && items[index].kind == ITEM_SUBMENU) open_submenu(level, index);
    else close_levels(level + 1);
}

static bool mouse_event(const SDL_Event *event) {
    float x, y;
    bool motion = event->type == SDL_EVENT_MOUSE_MOTION;
    SDL_WindowID id = motion ? event->motion.windowID : event->button.windowID;
    float event_x = motion ? event->motion.x : event->button.x, event_y = motion ? event->motion.y : event->button.y;
    bool in_main = main_window && id == SDL_GetWindowID(main_window);
    if (!to_main_point(id, event_x, event_y, &x, &y)) return !in_main;
    if (!depth) {
        if (!in_main) return true;
        if (event->type != SDL_EVENT_MOUSE_BUTTON_DOWN || y >= BAR_HEIGHT) return false;
        int top = bar_hit(x, y);
        if (top >= 0) open_top_menu(top, false);
        return true;
    }
    if (motion) {
        hover_at(x, y);
        return true;
    }
    int index;
    int level = level_hit(x, y, &index);
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int top = bar_hit(x, y);
        if (top >= 0) {
            if (top == open_top) close_levels(0);
            else open_top_menu(top, false);
        } else if (level < 0) {
            close_levels(0);
        }
        return true;
    }
    if (level >= 0 && index >= 0 && items[index].kind == ITEM_ACTION) activate(index);
    return true;
}

static void move_top(int step) {
    if (top_count) open_top_menu(((open_top + step) % top_count + top_count) % top_count, true);
}

static bool key_event(const SDL_Event *event) {
    if (!depth) {
        if (event->type != SDL_EVENT_KEY_DOWN) return false;
        SDL_Keycode key = SDL_GetKeyFromScancode(event->key.scancode, SDL_KMOD_NONE, false);
        SDL_Keymod modifiers = SDL_KMOD_NONE;
        if (event->key.mod & SDL_KMOD_CTRL) modifiers |= SDL_KMOD_CTRL;
        if (event->key.mod & SDL_KMOD_ALT) modifiers |= SDL_KMOD_ALT;
        if (event->key.mod & SDL_KMOD_SHIFT) modifiers |= SDL_KMOD_SHIFT;
        if (key == SDLK_F10 && modifiers == SDL_KMOD_NONE) {
            if (!event->key.repeat) open_top_menu(0, true);
            return true;
        }
        for (int i = 0; i < item_count; i++) {
            if (!items[i].key || items[i].key != key || items[i].modifiers != modifiers) continue;
            if (!event->key.repeat && enabled[items[i].tag] && queued < MENU_QUEUE) queue[queued++] = items[i].tag;
            return true;
        }
        return false;
    }
    if (event->type != SDL_EVENT_KEY_DOWN) return true;
    level_t *level = &levels[depth - 1];
    switch (event->key.key) {
    case SDLK_ESCAPE:
    case SDLK_F10:
        close_levels(event->key.key == SDLK_ESCAPE && depth > 1 ? depth - 1 : 0);
        break;
    case SDLK_UP:
    case SDLK_DOWN: {
        bool down = event->key.key == SDLK_DOWN;
        int from = level->hover;
        if (from < 0) from = down ? menus[level->menu].first - 1 : menus[level->menu].first;
        int next = first_selectable(level->menu, from, down ? 1 : -1);
        if (next >= 0) level->hover = next;
        close_levels((int)(level - levels) + 1);
        break;
    }
    case SDLK_RIGHT:
        if (level->hover >= 0 && items[level->hover].kind == ITEM_SUBMENU) activate(level->hover);
        else move_top(1);
        break;
    case SDLK_LEFT:
        if (depth > 1) close_levels(depth - 1);
        else move_top(-1);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        if (level->hover >= 0) activate(level->hover);
        break;
    default:
        break;
    }
    return true;
}

void menu_install(SDL_Window *window) {
    main_window = window;
    for (int i = 0; i < MENU_COUNT; i++) {
        item_of_tag[i] = -1;
        enabled[i] = true;
    }
    for (int cursor = 0; cursor < MENU_ENTRY_COUNT;) {
        const menu_entry_t *entry = &MENU_ENTRIES[cursor++];
        if (entry->kind != MENU_ENTRY_MENU) continue;
        int menu = parse_menu(&cursor, entry->title);
        if (menu >= 0) top_menus[top_count++] = menu;
    }
    load_font();
    bake_font();
}

void menu_ensure(void) {
}

int menu_bar_height(void) {
    return (int)BAR_HEIGHT;
}

void menu_insets(int *left, int *top, int *right, int *bottom) {
    *left = *right = *bottom = 0;
    *top = menu_bar_height();
}

bool menu_active(void) {
    return depth > 0;
}

bool menu_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return mouse_event(event);
    case SDL_EVENT_MOUSE_WHEEL:
        return level_of_window(event->wheel.windowID) >= 0;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return key_event(event);
    default:
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST && level_of_window(event->window.windowID) >= 0) {
            if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) close_levels(0);
            return true;
        }
        return false;
    }
}

static void draw_bar(SDL_Renderer *renderer) {
    const palette_t *colours = palette();
    bar_canvas.renderer = renderer;
    int width = 0, height = 0;
    SDL_GetWindowSize(main_window, &width, &height);
    layout_bar();
    fill_rect(&bar_canvas, 0, 0, (float)width, BAR_HEIGHT, colours->bar);
    fill_rect(&bar_canvas, 0, BAR_HEIGHT - 1, (float)width, 1, colours->bar_border);
    for (int i = 0; i < top_count; i++) {
        const menu_t *menu = &menus[top_menus[i]];
        if (i == open_top) fill_rect(&bar_canvas, menu->bar_x, 0, menu->bar_width, BAR_HEIGHT - 1, colours->bar_open);
        draw_text(&bar_canvas, menu->bar_x + BAR_PADDING, 0, BAR_HEIGHT - 1, menu->title, colours->text);
    }
}

static void draw_level(level_t *level) {
    const palette_t *colours = palette();
    canvas_t *canvas = &level->canvas;
    float width, height;
    measure_menu(level->menu, &width, &height);
    if (width != level->width || height != level->height) {
        level->width = width;
        level->height = height;
        SDL_SetWindowSize(canvas->window, (int)width, (int)height);
    }
    fill_rect(canvas, 0, 0, width, height, colours->menu_border);
    fill_rect(canvas, 1, 1, width - 2, height - 2, colours->menu);
    float top = MENU_PADDING;
    const menu_t *menu = &menus[level->menu];
    for (int i = menu->first; i < menu->first + menu->count; i++) {
        const item_t *item = &items[i];
        if (!item_visible(item)) continue;
        if (item->kind == ITEM_SEPARATOR) {
            fill_rect(canvas, 1, top + floorf(SEPARATOR_HEIGHT / 2), width - 2, 1, colours->separator);
            top += SEPARATOR_HEIGHT;
            continue;
        }
        if (item->kind == ITEM_HEADING) {
            draw_text(canvas, HEADING_INSET, top, HEADING_HEIGHT, item->title, colours->shortcut);
            top += HEADING_HEIGHT;
            continue;
        }
        bool selectable = item_selectable(item);
        bool highlighted = selectable && level->hover == i;
        if (highlighted) fill_rect(canvas, 1, top, width - 2, ROW_HEIGHT, colours->highlight);
        colour_t text = !selectable ? colours->disabled : highlighted ? colours->highlight_text : colours->text;
        if (item->kind == ITEM_ACTION && checked[item->tag]) draw_mark(canvas, GLYPH_CHECK, CHECK_COLUMN / 2 + 1, top, text);
        draw_text(canvas, CHECK_COLUMN, top, ROW_HEIGHT, item->title, text);
        if (item->shortcut[0]) {
            colour_t shortcut = !selectable ? colours->disabled : highlighted ? colours->highlight_text : colours->shortcut;
            draw_text(canvas, width - TEXT_RIGHT - text_width(item->shortcut), top, ROW_HEIGHT, item->shortcut, shortcut);
        }
        if (item->kind == ITEM_SUBMENU) draw_mark(canvas, GLYPH_ARROW, width - ARROW_COLUMN / 2, top, text);
        top += ROW_HEIGHT;
    }
}

static bool focus_is_ours(void) {
    SDL_Window *focus = SDL_GetKeyboardFocus();
    return focus && (focus == main_window || level_of_window(SDL_GetWindowID(focus)) >= 0);
}

static bool inside(float x, float y, float left, float top, float width, float height) {
    return x >= left && y >= top && x < left + width && y < top + height;
}

static bool clicked_elsewhere(void) {
    const char *driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "x11")) return false;
    float x, y;
    if (!SDL_GetGlobalMouseState(&x, &y)) return false;
    int window_x, window_y, width, height;
    SDL_GetWindowPosition(main_window, &window_x, &window_y);
    SDL_GetWindowSize(main_window, &width, &height);
    x -= (float)window_x;
    y -= (float)window_y;
    if (inside(x, y, 0, 0, (float)width, (float)height)) return false;
    for (int level = 0; level < depth; level++) {
        float origin_x, origin_y;
        level_origin(level, &origin_x, &origin_y);
        if (inside(x, y, origin_x, origin_y, levels[level].width, levels[level].height)) return false;
    }
    return true;
}

void menu_draw(SDL_Renderer *renderer) {
    if (!main_window) return;
    if (depth && (!focus_is_ours() || clicked_elsewhere())) close_levels(0);
    bake_font();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    draw_bar(renderer);
    for (int i = 0; i < depth; i++) {
        SDL_Renderer *popup = levels[i].canvas.renderer;
        SDL_SetRenderDrawBlendMode(popup, SDL_BLENDMODE_BLEND);
        draw_level(&levels[i]);
        SDL_RenderPresent(popup);
    }
}

int menu_poll(void) {
    if (queued == 0) return -1;
    int item = queue[0];
    for (int i = 1; i < queued; i++) queue[i - 1] = queue[i];
    queued--;
    return item;
}

void menu_set_enabled(int item, bool on) {
    if (item >= 0 && item < MENU_COUNT) enabled[item] = on;
}

void menu_set_checked(int item, bool on) {
    if (item >= 0 && item < MENU_COUNT) checked[item] = on;
}

void menu_set_title(int item, const char *title) {
    if (item < 0 || item >= MENU_COUNT || item_of_tag[item] < 0) return;
    snprintf(items[item_of_tag[item]].title, sizeof items[0].title, "%s", title);
}

void menu_set_hidden(int item, bool on) {
    if (item >= 0 && item < MENU_COUNT) hidden[item] = on;
}

int menu_modifiers(void) {
    return 0;
}

#define FORM_WIDTH      500.0f
#define FORM_PADDING    18.0f
#define FORM_LABEL      92.0f
#define FORM_GAP        8.0f
#define FORM_ROW        30.0f
#define FORM_CONTROL    24.0f
#define FORM_LINE       18.0f
#define FORM_BUTTON     88.0f
#define FORM_WIDGETS    8
#define FORM_OPTIONS    40
#define FORM_LIST_ROW   22.0f
#define FORM_CANCEL     -1

typedef enum { WIDGET_TEXT, WIDGET_CHOICE, WIDGET_CHECK, WIDGET_BUTTON } widget_kind_t;

typedef struct {
    widget_kind_t kind;
    const char   *label;
    const char   *text;
    char         *value;
    size_t value_size;
    const char   *placeholder;
    const char   *options[FORM_OPTIONS];
    int count, selected;
    uint64_t enabled;
    bool checked;
    int result;
    float x, y, width, height;
} widget_t;

typedef struct form form_t;

struct form {
    const char *title, *message;
    widget_t widgets[FORM_WIDGETS];
    int count, focus, open, hover, scroll, default_result;
    float x, y, width, height;
    void (*changed)(form_t *form, int widget, void *context);
    void       *context;
};

static int add_widget(form_t *form, widget_kind_t kind, const char *label) {
    widget_t *widget = &form->widgets[form->count];
    memset(widget, 0, sizeof *widget);
    widget->kind = kind;
    widget->label = label;
    widget->enabled = ~0ull;
    return form->count++;
}

static int wrap_lines(const char *text, float width, char lines[][160], int max) {
    int count = 0;
    const char *at = text;
    while (*at && count < max) {
        const char *end = at, *fit = at;
        while (*end) {
            const char *word = end;
            while (*word == ' ') word++;
            while (*word && *word != ' ') word++;
            char candidate[160];
            snprintf(candidate, sizeof candidate, "%.*s", (int)(word - at), at);
            if (text_width(candidate) > width && fit != at) break;
            fit = end = word;
        }
        snprintf(lines[count++], 160, "%.*s", (int)(fit - at), at);
        at = fit;
        while (*at == ' ') at++;
    }
    return count;
}

static void layout_form(form_t *form) {
    int window_width, window_height;
    SDL_GetWindowSize(main_window, &window_width, &window_height);
    char lines[8][160];
    int message_lines = form->message ? wrap_lines(form->message, FORM_WIDTH - 2 * FORM_PADDING, lines, 8) : 0;
    float y = FORM_PADDING + FORM_LINE + FORM_GAP + message_lines * FORM_LINE + FORM_GAP;
    float buttons = 0;
    for (int i = 0; i < form->count; i++) {
        widget_t *widget = &form->widgets[i];
        if (widget->kind == WIDGET_BUTTON) {
            buttons++;
            continue;
        }
        widget->x = FORM_PADDING + FORM_LABEL + FORM_GAP;
        widget->y = y + (FORM_ROW - FORM_CONTROL) / 2;
        widget->width = FORM_WIDTH - widget->x - FORM_PADDING;
        widget->height = FORM_CONTROL;
        y += FORM_ROW;
    }
    y += FORM_GAP;
    float right = FORM_WIDTH - FORM_PADDING;
    for (int i = form->count - 1; i >= 0; i--) {
        widget_t *widget = &form->widgets[i];
        if (widget->kind != WIDGET_BUTTON) continue;
        widget->width = FORM_BUTTON;
        widget->height = FORM_CONTROL + 4;
        widget->x = right - FORM_BUTTON;
        widget->y = y;
        right -= FORM_BUTTON + FORM_GAP;
    }
    form->width = FORM_WIDTH;
    form->height = y + (buttons ? FORM_CONTROL + 4 : 0) + FORM_PADDING;
    form->x = floorf(((float)window_width - form->width) / 2);
    form->y = floorf(BAR_HEIGHT + ((float)window_height - BAR_HEIGHT - form->height) / 2);
    if (form->y < BAR_HEIGHT) form->y = BAR_HEIGHT;
    if (form->x < 0) form->x = 0;
}

static bool option_enabled(const widget_t *widget, int option) {
    return option < 64 ? (widget->enabled >> option) & 1u : true;
}

static int visible_options(const form_t *form) {
    int window_width, window_height;
    SDL_GetWindowSize(main_window, &window_width, &window_height);
    const widget_t *widget = &form->widgets[form->open];
    float top = form->y + widget->y + widget->height;
    int rows = (int)(((float)window_height - top - 4) / FORM_LIST_ROW);
    if (rows < 3) rows = 3;
    return rows < widget->count ? rows : widget->count;
}

static void draw_form(form_t *form) {
    const palette_t *colours = palette();
    canvas_t *canvas = &bar_canvas;
    canvas->renderer = SDL_GetRenderer(main_window);
    SDL_SetRenderDrawBlendMode(canvas->renderer, SDL_BLENDMODE_BLEND);
    bake_font();
    layout_form(form);
    int window_width, window_height;
    SDL_GetWindowSize(main_window, &window_width, &window_height);
    fill_rect(canvas, 0, 0, (float)window_width, (float)window_height, colours->bar_open);
    draw_bar(canvas->renderer);
    float ox = form->x, oy = form->y;
    fill_rect(canvas, ox, oy, form->width, form->height, colours->menu_border);
    fill_rect(canvas, ox + 1, oy + 1, form->width - 2, form->height - 2, colours->menu);
    draw_text(canvas, ox + FORM_PADDING, oy + FORM_PADDING, FORM_LINE, form->title, colours->text);
    char lines[8][160];
    int message_lines = form->message ? wrap_lines(form->message, FORM_WIDTH - 2 * FORM_PADDING, lines, 8) : 0;
    for (int i = 0; i < message_lines; i++) draw_text(canvas, ox + FORM_PADDING, oy + FORM_PADDING + FORM_LINE + FORM_GAP + i * FORM_LINE, FORM_LINE, lines[i], colours->shortcut);
    for (int i = 0; i < form->count; i++) {
        widget_t *widget = &form->widgets[i];
        float x = ox + widget->x, y = oy + widget->y;
        bool focused = form->focus == i;
        if (widget->label) {
            float label_width = text_width(widget->label);
            draw_text(canvas, ox + FORM_PADDING + FORM_LABEL - label_width, y, widget->height, widget->label, colours->text);
        }
        colour_t border = focused ? colours->highlight : colours->menu_border;
        switch (widget->kind) {
        case WIDGET_TEXT:
            fill_rect(canvas, x, y, widget->width, widget->height, border);
            fill_rect(canvas, x + 1, y + 1, widget->width - 2, widget->height - 2, colours->menu);
            if (widget->value[0]) draw_text(canvas, x + 6, y, widget->height, widget->value, colours->text);
            else if (widget->placeholder) draw_text(canvas, x + 6, y, widget->height, widget->placeholder, colours->disabled);
            if (focused) fill_rect(canvas, x + 6 + text_width(widget->value) + 1, y + 5, 1, widget->height - 10, colours->text);
            break;
        case WIDGET_CHOICE:
            fill_rect(canvas, x, y, widget->width, widget->height, border);
            fill_rect(canvas, x + 1, y + 1, widget->width - 2, widget->height - 2, colours->bar);
            if (widget->selected >= 0 && widget->selected < widget->count) draw_text(canvas, x + 6, y, widget->height, widget->options[widget->selected], colours->text);
            draw_mark(canvas, GLYPH_ARROW, x + widget->width - 12, y + (widget->height - ROW_HEIGHT) / 2, colours->text);
            break;
        case WIDGET_CHECK:
            fill_rect(canvas, x, y + 4, 16, 16, border);
            fill_rect(canvas, x + 1, y + 5, 14, 14, colours->menu);
            if (widget->checked) draw_mark(canvas, GLYPH_CHECK, x + 8, y + (widget->height - ROW_HEIGHT) / 2, colours->text);
            draw_text(canvas, x + 24, y, widget->height, widget->text, colours->text);
            break;
        case WIDGET_BUTTON: {
            bool primary = widget->result == form->default_result;
            fill_rect(canvas, x, y, widget->width, widget->height, focused ? colours->highlight : colours->menu_border);
            fill_rect(canvas, x + 1, y + 1, widget->width - 2, widget->height - 2, primary ? colours->highlight : colours->bar);
            float label_width = text_width(widget->text);
            draw_text(canvas, x + (widget->width - label_width) / 2, y, widget->height, widget->text, primary ? colours->highlight_text : colours->text);
            break;
        }
        }
    }
    if (form->open >= 0) {
        widget_t *widget = &form->widgets[form->open];
        int rows = visible_options(form);
        float x = ox + widget->x, y = oy + widget->y + widget->height;
        fill_rect(canvas, x, y, widget->width, rows * FORM_LIST_ROW + 2, colours->menu_border);
        fill_rect(canvas, x + 1, y + 1, widget->width - 2, rows * FORM_LIST_ROW, colours->menu);
        for (int row = 0; row < rows; row++) {
            int option = form->scroll + row;
            if (option >= widget->count) break;
            bool enabled_option = option_enabled(widget, option);
            bool highlighted = enabled_option && option == form->hover;
            float top = y + 1 + row * FORM_LIST_ROW;
            if (highlighted) fill_rect(canvas, x + 1, top, widget->width - 2, FORM_LIST_ROW, colours->highlight);
            colour_t text = !enabled_option ? colours->disabled : highlighted ? colours->highlight_text : colours->text;
            draw_text(canvas, x + 6, top, FORM_LIST_ROW, widget->options[option], text);
        }
    }
    SDL_RenderPresent(canvas->renderer);
}

static int widget_at(const form_t *form, float x, float y) {
    for (int i = 0; i < form->count; i++) {
        const widget_t *widget = &form->widgets[i];
        float left = form->x + widget->x, top = form->y + widget->y;
        float width = widget->kind == WIDGET_CHECK ? 24 + text_width(widget->text) : widget->width;
        if (inside(x, y, left, top, width, widget->height)) return i;
    }
    return -1;
}

static int option_at(const form_t *form, float x, float y) {
    const widget_t *widget = &form->widgets[form->open];
    float left = form->x + widget->x, top = form->y + widget->y + widget->height + 1;
    int rows = visible_options(form);
    if (!inside(x, y, left, top, widget->width, rows * FORM_LIST_ROW)) return -1;
    int option = form->scroll + (int)((y - top) / FORM_LIST_ROW);
    return option < widget->count ? option : -1;
}

static void choose(form_t *form, int widget_index, int option) {
    widget_t *widget = &form->widgets[widget_index];
    if (option < 0 || option >= widget->count || !option_enabled(widget, option)) return;
    widget->selected = option;
    if (form->changed) form->changed(form, widget_index, form->context);
}

static void open_choice(form_t *form, int widget_index) {
    form->open = widget_index;
    form->hover = form->widgets[widget_index].selected;
    int rows = visible_options(form);
    form->scroll = form->hover >= rows ? form->hover - rows + 1 : 0;
}

static void step_choice(form_t *form, int widget_index, int step) {
    widget_t *widget = &form->widgets[widget_index];
    for (int option = widget->selected + step; option >= 0 && option < widget->count; option += step) {
        if (option_enabled(widget, option)) {
            choose(form, widget_index, option);
            return;
        }
    }
}

static void move_hover(form_t *form, int step) {
    widget_t *widget = &form->widgets[form->open];
    for (int option = form->hover + step; option >= 0 && option < widget->count; option += step) {
        if (!option_enabled(widget, option)) continue;
        form->hover = option;
        int rows = visible_options(form);
        if (option < form->scroll) form->scroll = option;
        if (option >= form->scroll + rows) form->scroll = option - rows + 1;
        return;
    }
}

static void remove_last_character(char *text) {
    size_t length = strlen(text);
    while (length && ((unsigned char)text[length - 1] & 0xc0) == 0x80) length--;
    if (length) length--;
    text[length] = 0;
}

static int press(form_t *form, int widget_index) {
    widget_t *widget = &form->widgets[widget_index];
    form->focus = widget_index;
    if (widget->kind == WIDGET_CHOICE) open_choice(form, widget_index);
    else if (widget->kind == WIDGET_CHECK) widget->checked = !widget->checked;
    else if (widget->kind == WIDGET_BUTTON) return widget->result;
    return INT32_MIN;
}

static int run_form(form_t *form) {
    form->open = -1;
    SDL_StartTextInput(main_window);
    int result = FORM_CANCEL;
    for (;;) {
        draw_form(form);
        SDL_Event event;
        if (!SDL_WaitEvent(&event)) break;
        int outcome = INT32_MIN;
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            SDL_PushEvent(&event);
            outcome = FORM_CANCEL;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (form->open >= 0) {
                int option = option_at(form, event.motion.x, event.motion.y);
                if (option >= 0 && option_enabled(&form->widgets[form->open], option)) form->hover = option;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (form->open >= 0) {
                int rows = visible_options(form), count = form->widgets[form->open].count;
                form->scroll -= event.wheel.y > 0 ? 1 : event.wheel.y < 0 ? -1 : 0;
                if (form->scroll > count - rows) form->scroll = count - rows;
                if (form->scroll < 0) form->scroll = 0;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (event.button.button != SDL_BUTTON_LEFT) break;
            if (form->open >= 0) {
                int option = option_at(form, event.button.x, event.button.y);
                int open = form->open;
                form->open = -1;
                if (option >= 0) choose(form, open, option);
                break;
            }
            {
                int widget_index = widget_at(form, event.button.x, event.button.y);
                if (widget_index >= 0) outcome = press(form, widget_index);
            }
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (form->open < 0 && form->widgets[form->focus].kind == WIDGET_TEXT) {
                widget_t *widget = &form->widgets[form->focus];
                size_t length = strlen(widget->value);
                snprintf(widget->value + length, widget->value_size - length, "%s", event.text.text);
            }
            break;
        case SDL_EVENT_KEY_DOWN: {
            SDL_Keycode key = event.key.key;
            widget_t *focused = &form->widgets[form->focus];
            if (form->open >= 0) {
                if (key == SDLK_ESCAPE) form->open = -1;
                else if (key == SDLK_UP) move_hover(form, -1);
                else if (key == SDLK_DOWN) move_hover(form, 1);
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
                    int open = form->open;
                    form->open = -1;
                    choose(form, open, form->hover);
                }
                break;
            }
            if (key == SDLK_ESCAPE) outcome = FORM_CANCEL;
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) outcome = focused->kind == WIDGET_BUTTON ? focused->result : form->default_result;
            else if (key == SDLK_TAB) form->focus = (form->focus + ((event.key.mod & SDL_KMOD_SHIFT) ? form->count - 1 : 1)) % form->count;
            else if (key == SDLK_BACKSPACE && focused->kind == WIDGET_TEXT) remove_last_character(focused->value);
            else if (focused->kind == WIDGET_CHOICE && (key == SDLK_UP || key == SDLK_DOWN)) step_choice(form, form->focus, key == SDLK_UP ? -1 : 1);
            else if (key == SDLK_SPACE && focused->kind != WIDGET_TEXT) outcome = press(form, form->focus);
            break;
        }
        }
        if (outcome != INT32_MIN) {
            result = outcome;
            break;
        }
    }
    SDL_StopTextInput(main_window);
    return result;
}

#define NEW_CREATE 1

static const char *SCREEN_LABELS[] = { "480 x 240 (original)", "640 x 240", "640 x 480", "800 x 600" };
static const char *MEMORY_LABELS[DIALOG_MEMORY_COUNT] = {
    "4 MB (original)", "8 MB", "16 MB", "20 MB (4 MB + 16 MB DRAM card)", "32 MB (16 MB + 16 MB DRAM card)",
};

typedef struct {
    dialog_rom_t roms[FORM_OPTIONS - 1];
    int count;
    dialog_probe_fn probe;
    int rom, screen, last_rom;
    SDL_AtomicInt picked;
    char picked_path[1024];
} new_machine_t;

static void update_screens(form_t *form, new_machine_t *state) {
    widget_t *rom = &form->widgets[state->rom], *screen = &form->widgets[state->screen];
    uint32_t mask = rom->selected < state->count ? state->roms[rom->selected].screens : 1u;
    screen->enabled = mask;
    if (!option_enabled(screen, screen->selected)) screen->selected = 0;
}

static void picked_rom(void *userdata, const char *const *files, int filter) {
    (void)filter;
    new_machine_t *state = userdata;
    if (files && files[0]) snprintf(state->picked_path, sizeof state->picked_path, "%s", files[0]);
    else state->picked_path[0] = 0;
    SDL_SetAtomicInt(&state->picked, 1);
}

static void rom_changed(form_t *form, int widget_index, void *context) {
    new_machine_t *state = context;
    if (widget_index != state->rom) return;
    widget_t *rom = &form->widgets[state->rom];
    if (rom->selected < state->count) {
        state->last_rom = rom->selected;
        update_screens(form, state);
        return;
    }
    SDL_SetAtomicInt(&state->picked, 0);
    SDL_ShowOpenFileDialog(picked_rom, state, main_window, NULL, 0, NULL, false);
    while (!SDL_GetAtomicInt(&state->picked)) {
        SDL_PumpEvents();
        SDL_Delay(20);
    }
    rom->selected = state->last_rom;
    char label[160];
    uint32_t screens = state->picked_path[0] && state->count < FORM_OPTIONS - 1 ? state->probe(state->picked_path, label, sizeof label) : 0;
    if (screens) {
        dialog_rom_t *added = &state->roms[state->count];
        snprintf(added->path, sizeof added->path, "%s", state->picked_path);
        snprintf(added->label, sizeof added->label, "%s", label);
        added->screens = screens;
        rom->options[state->count] = added->label;
        state->count++;
        rom->options[state->count] = "Other ROM File" ELLIPSIS;
        rom->count = state->count + 1;
        rom->selected = state->last_rom = state->count - 1;
    } else if (state->picked_path[0]) {
        const SDL_MessageBoxButtonData buttons[] = { { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "OK" } };
        const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_WARNING, main_window, "Not a Velo ROM", "That file isn't a ROM this emulator can run.", 1, buttons, NULL };
        int chosen;
        SDL_ShowMessageBox(&dialog, &chosen);
    }
    update_screens(form, state);
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    (void)window;
    static new_machine_t state;
    memset(&state, 0, sizeof state);
    state.probe = probe;
    state.count = rom_count < FORM_OPTIONS - 1 ? rom_count : FORM_OPTIONS - 1;
    memcpy(state.roms, roms, (size_t)state.count * sizeof roms[0]);
    form_t form = { "New Machine", "The screen, memory and clock settings are fixed for the life of the machine. The ROM decides which screen sizes are available.", { { 0 } }, 0, 0, -1, 0, 0, NEW_CREATE, 0, 0, 0, 0, rom_changed, &state };
    int name = add_widget(&form, WIDGET_TEXT, "Name:");
    form.widgets[name].value = result->name;
    form.widgets[name].value_size = sizeof result->name;
    form.widgets[name].placeholder = "Named from the settings below";
    state.rom = add_widget(&form, WIDGET_CHOICE, "Version:");
    widget_t *rom = &form.widgets[state.rom];
    for (int i = 0; i < state.count; i++) {
        rom->options[i] = state.roms[i].label;
        if (!strcmp(state.roms[i].path, result->rom)) rom->selected = state.last_rom = i;
    }
    rom->options[state.count] = "Other ROM File" ELLIPSIS;
    rom->count = state.count + 1;
    state.screen = add_widget(&form, WIDGET_CHOICE, "Screen:");
    widget_t *screen = &form.widgets[state.screen];
    for (int i = 0; i < SCREEN_PRESET_COUNT && i < (int)(sizeof SCREEN_LABELS / sizeof SCREEN_LABELS[0]); i++) {
        screen->options[screen->count++] = SCREEN_LABELS[i];
        if (SCREEN_PRESETS[i].width == result->screen.width && SCREEN_PRESETS[i].height == result->screen.height) screen->selected = i;
    }
    int memory = add_widget(&form, WIDGET_CHOICE, "Memory:");
    for (int i = 0; i < DIALOG_MEMORY_COUNT; i++) {
        form.widgets[memory].options[form.widgets[memory].count++] = MEMORY_LABELS[i];
        if (DIALOG_MEMORY_SIZES[i] == result->memory) form.widgets[memory].selected = i;
    }
    int clock = add_widget(&form, WIDGET_CHECK, NULL);
    form.widgets[clock].text = "Set the clock from this computer at the first boot";
    form.widgets[clock].checked = result->host_time;
    int cancel = add_widget(&form, WIDGET_BUTTON, NULL);
    form.widgets[cancel].text = "Cancel";
    form.widgets[cancel].result = FORM_CANCEL;
    int create = add_widget(&form, WIDGET_BUTTON, NULL);
    form.widgets[create].text = "Create";
    form.widgets[create].result = NEW_CREATE;
    update_screens(&form, &state);
    if (run_form(&form) != NEW_CREATE) return false;
    if (rom->selected >= state.count) return false;
    snprintf(result->rom, sizeof result->rom, "%s", state.roms[rom->selected].path);
    result->screen = SCREEN_PRESETS[screen->selected];
    result->memory = DIALOG_MEMORY_SIZES[form.widgets[memory].selected];
    result->host_time = form.widgets[clock].checked;
    return true;
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    (void)window;
    static char titles[FORM_OPTIONS][120];
    form_t form = { "Manage Machines", "Reset sets a machine back to its factory state. Delete removes it and its saved state. Both put a backup in Snapshots/Backups first. The running machine can be reset but not deleted.", { { 0 } }, 0, 0, -1, 0, 0, DIALOG_MANAGE_CLOSE, 0, 0, 0, 0, NULL, NULL };
    int list = add_widget(&form, WIDGET_CHOICE, "Machine:");
    for (int i = 0; i < count && i < FORM_OPTIONS; i++) {
        snprintf(titles[i], sizeof titles[i], "%s%s", names[i], i == current ? " (running)" : "");
        form.widgets[list].options[form.widgets[list].count++] = titles[i];
    }
    form.widgets[list].selected = *chosen >= 0 && *chosen < count ? *chosen : 0;
    static const struct { const char *title; int result; } BUTTONS[] = {
        { "Delete" ELLIPSIS, DIALOG_MANAGE_DELETE }, { "Reset" ELLIPSIS, DIALOG_MANAGE_RESET }, { "Done", DIALOG_MANAGE_CLOSE },
    };
    for (int i = 0; i < 3; i++) {
        int button = add_widget(&form, WIDGET_BUTTON, NULL);
        form.widgets[button].text = BUTTONS[i].title;
        form.widgets[button].result = BUTTONS[i].result;
    }
    int result = run_form(&form);
    *chosen = form.widgets[list].selected;
    return result == DIALOG_MANAGE_RESET || result == DIALOG_MANAGE_DELETE ? (dialog_manage_t)result : DIALOG_MANAGE_CLOSE;
}

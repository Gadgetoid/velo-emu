#include <SDL3/SDL.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app/android.h"
#include "app/dialog.h"
#include "app/menu.h"
#include "app/menu_layout.h"
#include "vendor/stb_truetype.h"

#define ROW_POINTS        44.0f
#define COLUMN_POINTS     64.0f
#define TAB_POINTS        28.0f
#define GAP_POINTS        3.0f
#define LIST_ROW_POINTS   48.0f
#define LIST_HEAD_POINTS  32.0f
#define LIST_SEP_POINTS   12.0f
#define LIST_PAD_POINTS   16.0f
#define DRAG_POINTS       8.0f
#define FLING_DECAY       4.0f
#define FLING_STOP_POINTS 20.0f
#define LABEL_WIDTH       0.85f
#define LABEL_HEIGHT      0.36f
#define LIST_LABEL_HEIGHT 0.36f
#define HEADING_SIZE      0.8f
#define MARK_SIZE         0.6f
#define FONT_SIZES        6
#define ATLAS_WIDTH       1024
#define ATLAS_HEIGHT      1024
#define FIRST_CHARACTER   32
#define CHARACTER_COUNT   95
#define ELLIPSIS_CODE     0x2026
#define STORAGE_ROOT      "/storage/emulated/0"
#define PROGRESS_WIDTH    0.6f
#define MENU_QUEUE        16
#define PAGE_MAX          8
#define ROW_MAX           128
#define TITLE_MAX         96
#define TAB_MAX           (PAGE_MAX + 1)
#define MODAL_WAIT_MS     16

typedef enum { BUTTON_TOGGLE, BUTTON_MENU, BUTTON_KEYBOARD, BUTTON_KEY, BUTTON_MODIFIER, BUTTON_ITEM } button_kind_t;

typedef struct {
    const char   *label;
    button_kind_t kind;
    SDL_Keycode   key;
    int           item;
} button_t;

static const button_t BUTTONS[] = {
    { "Hide", BUTTON_TOGGLE, 0, 0 },
    { "Menu", BUTTON_MENU, 0, 0 },
    { "Esc", BUTTON_KEY, SDLK_ESCAPE, 0 },
    { "Tab", BUTTON_KEY, SDLK_TAB, 0 },
    { "Ctrl", BUTTON_MODIFIER, SDLK_LCTRL, 0 },
    { "Alt", BUTTON_MODIFIER, SDLK_LALT, 0 },
    { "Shift", BUTTON_MODIFIER, SDLK_LSHIFT, 0 },
    { "Kbd", BUTTON_KEYBOARD, 0, 0 },
    { "Power", BUTTON_ITEM, 0, MENU_POWER },
    { "Light", BUTTON_ITEM, 0, MENU_BACKLIGHT },
    { "^", BUTTON_KEY, SDLK_UP, 0 },
    { "<", BUTTON_KEY, SDLK_LEFT, 0 },
    { ">", BUTTON_KEY, SDLK_RIGHT, 0 },
    { "v", BUTTON_KEY, SDLK_DOWN, 0 },
    { "Enter", BUTTON_KEY, SDLK_RETURN, 0 },
};

#define BUTTON_COUNT (int)(sizeof BUTTONS / sizeof BUTTONS[0])
#define FIRST_GROUP  8

static const int UNSUPPORTED[] = {
    MENU_SHOW_DEBUG_OUTPUT, MENU_SHOW_STATE, MENU_SCALE_50, MENU_SCALE_75, MENU_SCALE_100, MENU_SCALE_150,
    MENU_SCALE_200, MENU_ZOOM_IN, MENU_ZOOM_OUT, MENU_FULL_SCREEN, MENU_SERIAL_PTY,
};

typedef struct {
    SDL_FRect buttons[BUTTON_COUNT];
    int       left, top, right, bottom;
    float     label_size;
} layout_t;

typedef enum { ROW_ITEM, ROW_HEADING, ROW_SEPARATOR } row_kind_t;

typedef struct {
    row_kind_t  kind;
    int         tag;
    const char *title;
    bool        checked, disabled;
} row_t;

typedef struct {
    const char *tabs[TAB_MAX];
    int         tab_count, tab_selected;
    row_t       rows[ROW_MAX];
    int         row_count;
} list_t;

typedef struct {
    bool tapped;
    int  tab, tag;
} tap_t;

static SDL_Window *main_window;
static bool        collapsed;
static int         pressed = -1;
static bool        latched[BUTTON_COUNT];
static bool        checked[MENU_COUNT];
static bool        hidden[MENU_COUNT];
static bool        disabled[MENU_COUNT];
static char        titles[MENU_COUNT][TITLE_MAX];
static int         queue[MENU_QUEUE];
static int         queued;

static bool  panel_open;
static int   page;
static float scroll;
static bool  touching, dragging;
static float touch_x, touch_y, touch_scroll;
static float velocity, last_motion_y;
static uint64_t last_motion_ns, last_frame_ns;

typedef struct {
    float            size;
    float            ascent;
    stbtt_packedchar characters[CHARACTER_COUNT];
    stbtt_packedchar ellipsis;
    SDL_Texture     *texture;
    uint64_t         used;
} font_size_t;

static struct {
    unsigned char *data;
    stbtt_fontinfo info;
    bool           tried, loaded;
    font_size_t    sizes[FONT_SIZES];
    uint64_t       clock;
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

static float text_width(SDL_Renderer *renderer, const char *text, float size) {
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

static void draw_text(SDL_Renderer *renderer, float x, float y, const char *text, float size) {
    font_size_t *entry = font_at(renderer, size);
    if (!entry) {
        char ascii[TITLE_MAX * 2];
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

static void draw_label(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float size) {
    float width = text_width(renderer, label, size);
    draw_text(renderer, rect->x + (rect->w - width) / 2, rect->y + (rect->h - size) / 2, label, size);
}

static float fit_size(SDL_Renderer *renderer, const SDL_FRect *rect, const char *label, float height) {
    float size = rect->h * height;
    float width = text_width(renderer, label, size);
    if (width > rect->w * LABEL_WIDTH) size *= rect->w * LABEL_WIDTH / width;
    return floorf(size);
}

static float display_scale(void) {
    float scale = main_window ? SDL_GetWindowDisplayScale(main_window) : 1.0f;
    return scale > 0 ? scale : 1.0f;
}

static SDL_FRect inset(SDL_FRect rect, float gap) {
    return (SDL_FRect){ floorf(rect.x + gap / 2), floorf(rect.y + gap / 2), floorf(rect.w - gap), floorf(rect.h - gap) };
}

static bool contains(const SDL_FRect *rect, float x, float y) {
    return rect->w > 0 && x >= rect->x && x < rect->x + rect->w && y >= rect->y && y < rect->y + rect->h;
}

static const char *label_of(int index) {
    return BUTTONS[index].kind == BUTTON_TOGGLE && collapsed ? "Keys" : BUTTONS[index].label;
}

static SDL_FRect cell_in_group(int index, float along, float across, float depth, bool columns) {
    bool first = index < FIRST_GROUP;
    int position = first ? index : index - FIRST_GROUP;
    float size = along / (first ? FIRST_GROUP : BUTTON_COUNT - FIRST_GROUP);
    if (columns) return (SDL_FRect){ first ? 0 : across - depth, position * size, depth, size };
    return (SDL_FRect){ position * size, first ? 0 : depth, size, depth };
}

static layout_t layout(void) {
    layout_t result = { 0 };
    if (!main_window) return result;
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(main_window, &safe);
    float scale = display_scale();
    float gap = GAP_POINTS * scale, tab = floorf(TAB_POINTS * scale);
    bool landscape = width > height;
    int start = landscape ? safe.x : safe.y;
    int end = landscape ? width - safe.x - safe.w : height - safe.y - safe.h;
    float along = (float)(landscape ? height : width), across = (float)((landscape ? width : height) - start - end);
    float depth = floorf((landscape ? COLUMN_POINTS : ROW_POINTS) * scale);
    int leading = start + (int)(collapsed ? tab : depth);
    int trailing = end + (landscape && !collapsed ? (int)depth : 0);
    if (!landscape && !collapsed) leading = start + (int)(2 * depth);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (collapsed && BUTTONS[i].kind != BUTTON_TOGGLE) continue;
        SDL_FRect cell = cell_in_group(i, along, across, collapsed ? tab : depth, landscape);
        if (landscape) cell.x += start;
        else cell.y += start;
        result.buttons[i] = inset(cell, gap);
    }
    if (landscape) {
        result.left = leading;
        result.right = trailing;
    } else {
        result.top = leading;
        result.bottom = trailing;
    }
    SDL_Renderer *renderer = SDL_GetRenderer(main_window);
    result.label_size = INFINITY;
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &result.buttons[i];
        if (rect->w > 0) result.label_size = fminf(result.label_size, fit_size(renderer, rect, label_of(i), LABEL_HEIGHT));
    }
    result.label_size = fmaxf(1, result.label_size);
    return result;
}

static int button_at(float x, float y) {
    layout_t current = layout();
    for (int i = 0; i < BUTTON_COUNT; i++) {
        if (contains(&current.buttons[i], x, y)) return i;
    }
    return -1;
}

static bool supported(int tag) {
    if (tag >= MENU_SERIAL_PORT_FIRST && tag <= MENU_SERIAL_PORT_LAST) return false;
    for (size_t i = 0; i < sizeof UNSUPPORTED / sizeof UNSUPPORTED[0]; i++) {
        if (UNSUPPORTED[i] == tag) return false;
    }
    return true;
}

static int page_entries(int *starts) {
    int count = 0;
    for (int i = 0; i < MENU_ENTRY_COUNT && count < PAGE_MAX; i++) {
        if (MENU_ENTRIES[i].kind == MENU_ENTRY_MENU) starts[count++] = i;
    }
    return count;
}

static bool visible_item(const menu_entry_t *entry) {
    return entry->kind == MENU_ENTRY_ITEM && supported(entry->tag) && !hidden[entry->tag];
}

static bool section_has_items(int from) {
    for (int i = from; i < MENU_ENTRY_COUNT; i++) {
        menu_entry_kind_t kind = MENU_ENTRIES[i].kind;
        if (kind == MENU_ENTRY_END || kind == MENU_ENTRY_SEPARATOR || kind == MENU_ENTRY_HEADING || kind == MENU_ENTRY_SUBMENU) return false;
        if (visible_item(&MENU_ENTRIES[i])) return true;
    }
    return false;
}

static void add_row(list_t *list, row_kind_t kind, int tag, const char *title, bool checked, bool disabled) {
    if (list->row_count < ROW_MAX) list->rows[list->row_count++] = (row_t){ kind, tag, title, checked, disabled };
}

static void panel_list(list_t *list) {
    int starts[PAGE_MAX];
    int pages = page_entries(starts);
    list->tab_count = pages + 1;
    list->tab_selected = page;
    list->row_count = 0;
    for (int i = 0; i < pages; i++) list->tabs[i] = MENU_ENTRIES[starts[i]].title;
    list->tabs[pages] = "Close";
    if (page < 0 || page >= pages) return;
    int depth = 0;
    for (int i = starts[page] + 1; i < MENU_ENTRY_COUNT; i++) {
        const menu_entry_t *entry = &MENU_ENTRIES[i];
        if (entry->kind == MENU_ENTRY_END) {
            if (depth-- == 0) break;
            continue;
        }
        if (entry->kind == MENU_ENTRY_SUBMENU || entry->kind == MENU_ENTRY_HEADING) {
            if (entry->kind == MENU_ENTRY_SUBMENU) depth++;
            if (section_has_items(i + 1)) add_row(list, ROW_HEADING, 0, entry->title, false, false);
        } else if (entry->kind == MENU_ENTRY_SEPARATOR) {
            if (list->row_count && list->rows[list->row_count - 1].kind != ROW_SEPARATOR) add_row(list, ROW_SEPARATOR, 0, NULL, false, false);
        } else if (visible_item(entry)) {
            int tag = entry->tag;
            add_row(list, ROW_ITEM, tag, titles[tag][0] ? titles[tag] : entry->title, checked[tag], disabled[tag]);
        }
    }
    while (list->row_count && list->rows[list->row_count - 1].kind == ROW_SEPARATOR) list->row_count--;
}

static float row_height(row_kind_t kind) {
    float points = kind == ROW_ITEM ? LIST_ROW_POINTS : kind == ROW_HEADING ? LIST_HEAD_POINTS : LIST_SEP_POINTS;
    return floorf(points * display_scale());
}

static SDL_Rect list_area(void) {
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_Rect safe = { 0, 0, width, height };
    SDL_GetWindowSafeArea(main_window, &safe);
    if (width > height) return (SDL_Rect){ safe.x, 0, safe.w, height };
    return (SDL_Rect){ 0, safe.y, width, safe.h };
}

static SDL_FRect tab_rect(int index, int count) {
    SDL_Rect area = list_area();
    float cell = (float)area.w / count;
    return inset((SDL_FRect){ area.x + index * cell, (float)area.y, cell, floorf(ROW_POINTS * display_scale()) }, GAP_POINTS * display_scale());
}

static float list_top(void) {
    return (float)list_area().y + floorf(ROW_POINTS * display_scale()) + floorf(GAP_POINTS * display_scale());
}

static void list_reset(void) {
    scroll = velocity = 0;
    touching = dragging = false;
    last_frame_ns = 0;
}

static void clamp_scroll(const list_t *list) {
    float height = 0;
    for (int i = 0; i < list->row_count; i++) height += row_height(list->rows[i].kind);
    SDL_Rect area = list_area();
    float visible = (float)(area.y + area.h) - list_top();
    scroll = fminf(fmaxf(scroll, 0), fmaxf(0, height - visible));
}

static int row_at(const list_t *list, float y) {
    float top = list_top() - scroll;
    for (int i = 0; i < list->row_count; i++) {
        const row_t *row = &list->rows[i];
        float height = row_height(row->kind);
        if (y >= top && y < top + height) return row->kind == ROW_ITEM && !row->disabled ? row->tag : -1;
        top += height;
    }
    return -1;
}

static tap_t list_tap(const list_t *list, float x, float y) {
    tap_t tap = { true, -1, -1 };
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(i, list->tab_count);
        if (contains(&rect, x, y)) {
            tap.tab = i;
            return tap;
        }
    }
    if (y >= list_top()) tap.tag = row_at(list, y);
    return tap;
}

static tap_t list_event(const list_t *list, const SDL_Event *event) {
    tap_t tap = { false, -1, -1 };
    float threshold = DRAG_POINTS * display_scale();
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        touching = true;
        dragging = false;
        velocity = 0;
        touch_x = event->button.x;
        touch_y = event->button.y;
        break;
    case SDL_EVENT_MOUSE_MOTION: {
        if (!touching) break;
        if (!dragging && fabsf(event->motion.y - touch_y) > threshold && touch_y >= list_top()) {
            dragging = true;
            touch_y = last_motion_y = event->motion.y;
            touch_scroll = scroll;
            last_motion_ns = event->motion.timestamp;
        }
        if (!dragging) break;
        scroll = touch_scroll - (event->motion.y - touch_y);
        clamp_scroll(list);
        float seconds = (float)(event->motion.timestamp - last_motion_ns) / SDL_NS_PER_SECOND;
        if (seconds > 0) velocity = velocity * 0.5f + (last_motion_y - event->motion.y) / seconds * 0.5f;
        last_motion_y = event->motion.y;
        last_motion_ns = event->motion.timestamp;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (touching && !dragging) tap = list_tap(list, touch_x, touch_y);
        if (!dragging || (float)(event->button.timestamp - last_motion_ns) / SDL_NS_PER_SECOND > 0.1f) velocity = 0;
        touching = dragging = false;
        last_frame_ns = SDL_GetTicksNS();
        break;
    default:
        break;
    }
    return tap;
}

static void fling(const list_t *list) {
    uint64_t now = SDL_GetTicksNS();
    float seconds = last_frame_ns ? (float)(now - last_frame_ns) / SDL_NS_PER_SECOND : 0;
    last_frame_ns = now;
    if (touching || velocity == 0 || seconds <= 0) return;
    float before = scroll;
    scroll += velocity * seconds;
    clamp_scroll(list);
    velocity *= expf(-FLING_DECAY * seconds);
    if (scroll == before || fabsf(velocity) < FLING_STOP_POINTS * display_scale()) velocity = 0;
}

static void push_key(SDL_Keycode key, bool down) {
    SDL_Event event = { 0 };
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.windowID = main_window ? SDL_GetWindowID(main_window) : 0;
    event.key.key = key;
    event.key.down = down;
    SDL_PushEvent(&event);
}

static void enqueue(int item) {
    if (queued < MENU_QUEUE) queue[queued++] = item;
}

static void release(void) {
    if (pressed < 0) return;
    push_key(BUTTONS[pressed].key, false);
    pressed = -1;
}

static void open_panel(bool open) {
    panel_open = open;
    list_reset();
    if (open) {
        release();
        if (SDL_ScreenKeyboardShown(main_window)) SDL_StopTextInput(main_window);
    }
}

static void press(int index) {
    const button_t *button = &BUTTONS[index];
    switch (button->kind) {
    case BUTTON_TOGGLE:
        collapsed = !collapsed;
        break;
    case BUTTON_MENU:
        open_panel(true);
        break;
    case BUTTON_KEYBOARD:
        if (SDL_ScreenKeyboardShown(main_window)) SDL_StopTextInput(main_window);
        else SDL_StartTextInput(main_window);
        break;
    case BUTTON_KEY:
        pressed = index;
        push_key(button->key, true);
        break;
    case BUTTON_MODIFIER:
        latched[index] = !latched[index];
        push_key(button->key, latched[index]);
        break;
    case BUTTON_ITEM:
        enqueue(button->item);
        break;
    }
}

static bool panel_event(const SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key == SDLK_AC_BACK || event->key.key == SDLK_ESCAPE) open_panel(false);
        return true;
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        return false;
    default:
        if (event->type >= SDL_EVENT_USER) return false;
        if (event->type >= SDL_EVENT_WINDOW_FIRST && event->type <= SDL_EVENT_WINDOW_LAST) return false;
        break;
    }
    list_t list;
    panel_list(&list);
    tap_t tap = list_event(&list, event);
    if (!tap.tapped) return true;
    if (tap.tab == list.tab_count - 1) {
        open_panel(false);
    } else if (tap.tab >= 0 && tap.tab != page) {
        page = tap.tab;
        list_reset();
    } else if (tap.tag >= 0) {
        enqueue(tap.tag);
        open_panel(false);
    }
    return true;
}

void menu_install(SDL_Window *window) {
    main_window = window;
    for (int i = 0; i < MENU_COUNT; i++) titles[i][0] = 0;
    SDL_strlcpy(titles[MENU_COPY_SCREEN], "Share Screen" ELLIPSIS, sizeof titles[MENU_COPY_SCREEN]);
}

int menu_bar_height(void) {
    return layout().top;
}

void menu_insets(int *left, int *top, int *right, int *bottom) {
    layout_t current = layout();
    *left = current.left;
    *top = current.top;
    *right = current.right;
    *bottom = current.bottom;
}

bool menu_event(const SDL_Event *event) {
    if (panel_open) return panel_event(event);
    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int index = button_at(event->button.x, event->button.y);
        if (index < 0) return false;
        press(index);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (pressed < 0) return false;
        release();
        return true;
    case SDL_EVENT_MOUSE_MOTION:
        return pressed >= 0;
    case SDL_EVENT_KEY_DOWN:
        if (event->key.key != SDLK_AC_BACK) return false;
        open_panel(true);
        return true;
    case SDL_EVENT_KEY_UP:
        return event->key.key == SDLK_AC_BACK;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release();
        memset(latched, 0, sizeof latched);
        return false;
    default:
        return false;
    }
}

bool menu_active(void) {
    return panel_open;
}

static void draw_list(SDL_Renderer *renderer, const list_t *list) {
    fling(list);
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, (float)height });

    float scale = display_scale();
    float text_size = floorf(LIST_ROW_POINTS * scale * LIST_LABEL_HEIGHT);
    float heading_size = floorf(text_size * HEADING_SIZE);
    SDL_Rect area = list_area();
    float top = list_top();
    float pad = floorf(LIST_PAD_POINTS * scale);
    float mark = floorf(text_size * MARK_SIZE);
    float y = top - scroll;
    for (int i = 0; i < list->row_count; i++) {
        const row_t *row = &list->rows[i];
        float row_h = row_height(row->kind);
        if (row->kind == ROW_SEPARATOR) {
            SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
            SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + row_h / 2), area.w - 2 * pad, fmaxf(1, floorf(scale)) });
        } else if (row->kind == ROW_HEADING) {
            SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
            draw_text(renderer, area.x + pad, y + row_h - heading_size - floorf(4 * scale), row->title, heading_size);
        } else {
            if (row->checked) {
                SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
                SDL_RenderFillRect(renderer, &(SDL_FRect){ area.x + pad, floorf(y + (row_h - mark) / 2), mark, mark });
            }
            if (row->disabled) SDL_SetRenderDrawColor(renderer, 0x66, 0x66, 0x66, 0xFF);
            else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
            draw_text(renderer, area.x + 2 * pad + mark, floorf(y + (row_h - text_size) / 2), row->title, text_size);
        }
        y += row_h;
    }

    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ 0, 0, (float)width, top });
    float tab_size = text_size;
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(i, list->tab_count);
        tab_size = fminf(tab_size, fit_size(renderer, &rect, list->tabs[i], LIST_LABEL_HEIGHT * 1.2f));
    }
    for (int i = 0; i < list->tab_count; i++) {
        SDL_FRect rect = tab_rect(i, list->tab_count);
        bool lit = i == list->tab_selected;
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, &rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, &rect, list->tabs[i], tab_size);
    }
}

void menu_draw(SDL_Renderer *renderer) {
    layout_t current = layout();
    bool keyboard = SDL_ScreenKeyboardShown(main_window);
    for (int i = 0; i < BUTTON_COUNT; i++) {
        SDL_FRect *rect = &current.buttons[i];
        if (rect->w <= 0) continue;
        bool lit = i == pressed || latched[i] || (BUTTONS[i].kind == BUTTON_KEYBOARD && keyboard) || (BUTTONS[i].kind == BUTTON_ITEM && checked[BUTTONS[i].item]);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
        SDL_RenderFillRect(renderer, rect);
        if (lit) SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
        else SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
        draw_label(renderer, rect, label_of(i), current.label_size);
    }
    if (panel_open) {
        list_t list;
        panel_list(&list);
        draw_list(renderer, &list);
    }
}

void menu_ensure(void) {}

int menu_poll(void) {
    if (!queued) return -1;
    int item = queue[0];
    memmove(queue, queue + 1, (size_t)--queued * sizeof queue[0]);
    return item;
}

void menu_set_checked(int item, bool value) {
    if (item >= 0 && item < MENU_COUNT) checked[item] = value;
}

void menu_set_enabled(int item, bool value) {
    if (item >= 0 && item < MENU_COUNT) disabled[item] = !value;
}

void menu_set_title(int item, const char *title) {
    if (item >= 0 && item < MENU_COUNT) SDL_strlcpy(titles[item], title, sizeof titles[item]);
}

void menu_set_hidden(int item, bool value) {
    if (item >= 0 && item < MENU_COUNT) hidden[item] = value;
}

int menu_modifiers(void) {
    return 0;
}

static tap_t modal_step(const list_t *list, int cancel_tab) {
    SDL_Renderer *renderer = SDL_GetRenderer(main_window);
    draw_list(renderer, list);
    SDL_RenderPresent(renderer);
    tap_t tap = { false, -1, -1 };
    SDL_Event event;
    if (!SDL_WaitEventTimeout(&event, MODAL_WAIT_MS)) return tap;
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        SDL_PushEvent(&event);
        return (tap_t){ true, cancel_tab, -1 };
    case SDL_EVENT_KEY_DOWN:
        if (event.key.key == SDLK_AC_BACK || event.key.key == SDLK_ESCAPE) return (tap_t){ true, cancel_tab, -1 };
        return tap;
    default:
        return list_event(list, &event);
    }
}

enum { CHOICE_ROM = 0x1000, CHOICE_MEMORY = 0x2000, CHOICE_SCREEN = 0x3000, CHOICE_CLOCK = 0x4000, CHOICE_MACHINE = 0x5000 };

static bool rom_screen(const dialog_rom_t *rom, int preset) {
    return rom->screens & (1u << preset);
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    (void)window;
    (void)probe;
    if (rom_count <= 0) return false;
    int rom = 0;
    for (int i = 0; i < rom_count; i++) {
        if (!strcmp(roms[i].path, result->rom)) rom = i;
    }
    int screen = screen_preset_index(result->screen);
    if (screen < 0 || !rom_screen(&roms[rom], screen)) screen = 0;
    uint32_t memory = result->memory;
    bool host_time = result->host_time;
    static char labels[ROW_MAX][TITLE_MAX];
    list_reset();
    for (;;) {
        list_t list = { { "Cancel", "Create" }, 2, -1, { { 0 } }, 0 };
        int label = 0;
        add_row(&list, ROW_HEADING, 0, "ROM", false, false);
        for (int i = 0; i < rom_count; i++) add_row(&list, ROW_ITEM, CHOICE_ROM + i, roms[i].label, i == rom, false);
        add_row(&list, ROW_HEADING, 0, "Memory", false, false);
        for (int i = 0; i < DIALOG_MEMORY_COUNT && label < ROW_MAX; i++) {
            snprintf(labels[label], TITLE_MAX, "%u MB", (unsigned)DIALOG_MEMORY_SIZES[i]);
            add_row(&list, ROW_ITEM, CHOICE_MEMORY + i, labels[label++], DIALOG_MEMORY_SIZES[i] == memory, false);
        }
        add_row(&list, ROW_HEADING, 0, "Screen", false, false);
        for (int i = 0; i < SCREEN_PRESET_COUNT && label < ROW_MAX; i++) {
            if (!rom_screen(&roms[rom], i)) continue;
            snprintf(labels[label], TITLE_MAX, "%dx%d", SCREEN_PRESETS[i].width, SCREEN_PRESETS[i].height);
            add_row(&list, ROW_ITEM, CHOICE_SCREEN + i, labels[label++], i == screen, false);
        }
        add_row(&list, ROW_HEADING, 0, "Clock", false, false);
        add_row(&list, ROW_ITEM, CHOICE_CLOCK, "Set the clock from this phone", host_time, false);
        tap_t tap = modal_step(&list, 0);
        if (!tap.tapped) continue;
        if (tap.tab == 0) return false;
        if (tap.tab == 1) {
            result->name[0] = 0;
            SDL_strlcpy(result->rom, roms[rom].path, sizeof result->rom);
            result->screen = SCREEN_PRESETS[screen];
            result->memory = memory;
            result->host_time = host_time;
            return true;
        }
        if (tap.tag >= CHOICE_ROM && tap.tag < CHOICE_ROM + rom_count) {
            rom = tap.tag - CHOICE_ROM;
            if (!rom_screen(&roms[rom], screen)) screen = 0;
        } else if (tap.tag >= CHOICE_MEMORY && tap.tag < CHOICE_MEMORY + DIALOG_MEMORY_COUNT) {
            memory = DIALOG_MEMORY_SIZES[tap.tag - CHOICE_MEMORY];
        } else if (tap.tag >= CHOICE_SCREEN && tap.tag < CHOICE_SCREEN + SCREEN_PRESET_COUNT) {
            screen = tap.tag - CHOICE_SCREEN;
        } else if (tap.tag == CHOICE_CLOCK) {
            host_time = !host_time;
        }
    }
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    (void)window;
    (void)current;
    int selected = *chosen >= 0 && *chosen < count ? *chosen : 0;
    list_reset();
    for (;;) {
        list_t list = { { "Close", "Reset", "Delete" }, 3, -1, { { 0 } }, 0 };
        add_row(&list, ROW_HEADING, 0, "Machines", false, false);
        for (int i = 0; i < count; i++) add_row(&list, ROW_ITEM, CHOICE_MACHINE + i, names[i], i == selected, false);
        tap_t tap = modal_step(&list, 0);
        if (!tap.tapped) continue;
        if (tap.tab == 0) return DIALOG_MANAGE_CLOSE;
        if (tap.tab == 1 || tap.tab == 2) {
            *chosen = selected;
            return tap.tab == 1 ? DIALOG_MANAGE_RESET : DIALOG_MANAGE_DELETE;
        }
        if (tap.tag >= CHOICE_MACHINE && tap.tag < CHOICE_MACHINE + count) selected = tap.tag - CHOICE_MACHINE;
    }
}

void android_progress(const char *title, float fraction) {
    SDL_Renderer *renderer = main_window ? SDL_GetRenderer(main_window) : NULL;
    if (!renderer) return;
    SDL_PumpEvents();
    int width, height;
    SDL_GetWindowSize(main_window, &width, &height);
    float scale = display_scale();
    float size = floorf(LIST_ROW_POINTS * scale * LIST_LABEL_HEIGHT);
    SDL_SetRenderDrawColor(renderer, 0x10, 0x10, 0x10, 0xFF);
    SDL_RenderClear(renderer);
    float bar_w = floorf(width * PROGRESS_WIDTH), bar_h = floorf(8 * scale);
    float x = floorf((width - bar_w) / 2), y = floorf(height / 2.0f);
    SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0xEE, 0xFF);
    draw_text(renderer, floorf((width - text_width(renderer, title, size)) / 2), y - size - floorf(12 * scale), title, size);
    SDL_SetRenderDrawColor(renderer, 0x44, 0x44, 0x44, 0xFF);
    SDL_RenderFillRect(renderer, &(SDL_FRect){ x, y, bar_w, bar_h });
    SDL_SetRenderDrawColor(renderer, 0x8A, 0x9A, 0x6A, 0xFF);
    if (fraction >= 0) {
        SDL_RenderFillRect(renderer, &(SDL_FRect){ x, y, floorf(bar_w * fminf(fraction, 1)), bar_h });
    } else {
        float segment = floorf(bar_w / 4), phase = (float)(SDL_GetTicks() % 1500) / 1500.0f;
        SDL_RenderFillRect(renderer, &(SDL_FRect){ x + floorf((bar_w - segment) * phase), y, segment, bar_h });
    }
    SDL_RenderPresent(renderer);
}

static int compare_names(const void *a, const void *b) {
    return strcasecmp((const char *)a, (const char *)b);
}

static int list_folders(const char *path, char names[][TITLE_MAX], int max) {
    DIR *dir = opendir(path);
    if (!dir) return 0;
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) && count < max) {
        if (entry->d_name[0] == '.') continue;
        char full[1300];
        struct stat info;
        if (snprintf(full, sizeof full, "%s/%s", path, entry->d_name) >= (int)sizeof full || stat(full, &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        SDL_strlcpy(names[count++], entry->d_name, TITLE_MAX);
    }
    closedir(dir);
    qsort(names, (size_t)count, TITLE_MAX, compare_names);
    return count;
}

bool android_choose_folder(const char *title, const char *start, char *path, size_t size) {
    enum { TAB_CANCEL, TAB_UP, TAB_CHOOSE };
    static char names[ROW_MAX][TITLE_MAX];
    char current[1024], shown[1100];
    struct stat info;
    SDL_strlcpy(current, start && stat(start, &info) == 0 && S_ISDIR(info.st_mode) ? start : STORAGE_ROOT, sizeof current);
    list_reset();
    for (;;) {
        list_t list = { { "Cancel", "Up", "Choose" }, 3, -1, { { 0 } }, 0 };
        bool at_root = !strcmp(current, STORAGE_ROOT);
        if (!strncmp(current, STORAGE_ROOT, strlen(STORAGE_ROOT))) snprintf(shown, sizeof shown, "Phone%s", current + strlen(STORAGE_ROOT));
        else SDL_strlcpy(shown, current, sizeof shown);
        add_row(&list, ROW_HEADING, 0, title, false, false);
        add_row(&list, ROW_ITEM, -1, shown, true, true);
        int count = list_folders(current, names, ROW_MAX - 3);
        for (int i = 0; i < count; i++) add_row(&list, ROW_ITEM, CHOICE_MACHINE + i, names[i], false, false);
        tap_t tap = modal_step(&list, TAB_CANCEL);
        if (!tap.tapped) continue;
        if (tap.tab == TAB_CANCEL) return false;
        if (tap.tab == TAB_CHOOSE) {
            SDL_strlcpy(path, current, size);
            return true;
        }
        if (tap.tab == TAB_UP && !at_root) {
            char *slash = strrchr(current, '/');
            if (slash && slash != current) *slash = 0;
            list_reset();
        } else if (tap.tag >= CHOICE_MACHINE && tap.tag < CHOICE_MACHINE + count) {
            size_t length = strlen(current);
            snprintf(current + length, sizeof current - length, "/%s", names[tap.tag - CHOICE_MACHINE]);
            list_reset();
        }
    }
}

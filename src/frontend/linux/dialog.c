#include "frontend/common/dialog.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/common/menu.h"
#include "frontend/common/menu_layout.h"
#include "frontend/linux/ui.h"

#define MARK_ROW        24.0f
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
    SDL_Window *window;
    canvas_t canvas;
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
            if (ui_text_width(candidate) > width && fit != at) break;
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
    SDL_GetWindowSize(form->window, &window_width, &window_height);
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
    float bar = (float)menu_bar_height();
    form->y = floorf(bar + ((float)window_height - bar - form->height) / 2);
    if (form->y < bar) form->y = bar;
    if (form->x < 0) form->x = 0;
}

static bool option_enabled(const widget_t *widget, int option) {
    return option < 64 ? (widget->enabled >> option) & 1u : true;
}

static int visible_options(const form_t *form) {
    int window_width, window_height;
    SDL_GetWindowSize(form->window, &window_width, &window_height);
    const widget_t *widget = &form->widgets[form->open];
    float top = form->y + widget->y + widget->height;
    int rows = (int)(((float)window_height - top - 4) / FORM_LIST_ROW);
    if (rows < 3) rows = 3;
    return rows < widget->count ? rows : widget->count;
}

static void draw_form(form_t *form) {
    const palette_t *colours = ui_palette();
    canvas_t *canvas = &form->canvas;
    canvas->renderer = SDL_GetRenderer(form->window);
    SDL_SetRenderDrawBlendMode(canvas->renderer, SDL_BLENDMODE_BLEND);
    ui_prepare(form->window);
    layout_form(form);
    int window_width, window_height;
    SDL_GetWindowSize(form->window, &window_width, &window_height);
    ui_fill(canvas, 0, 0, (float)window_width, (float)window_height, colours->bar_open);
    menu_draw(canvas->renderer);
    float ox = form->x, oy = form->y;
    ui_fill(canvas, ox, oy, form->width, form->height, colours->menu_border);
    ui_fill(canvas, ox + 1, oy + 1, form->width - 2, form->height - 2, colours->menu);
    ui_text(canvas, ox + FORM_PADDING, oy + FORM_PADDING, FORM_LINE, form->title, colours->text);
    char lines[8][160];
    int message_lines = form->message ? wrap_lines(form->message, FORM_WIDTH - 2 * FORM_PADDING, lines, 8) : 0;
    for (int i = 0; i < message_lines; i++) ui_text(canvas, ox + FORM_PADDING, oy + FORM_PADDING + FORM_LINE + FORM_GAP + i * FORM_LINE, FORM_LINE, lines[i], colours->shortcut);
    for (int i = 0; i < form->count; i++) {
        widget_t *widget = &form->widgets[i];
        float x = ox + widget->x, y = oy + widget->y;
        bool focused = form->focus == i;
        if (widget->label) {
            float label_width = ui_text_width(widget->label);
            ui_text(canvas, ox + FORM_PADDING + FORM_LABEL - label_width, y, widget->height, widget->label, colours->text);
        }
        colour_t border = focused ? colours->highlight : colours->menu_border;
        switch (widget->kind) {
        case WIDGET_TEXT:
            ui_fill(canvas, x, y, widget->width, widget->height, border);
            ui_fill(canvas, x + 1, y + 1, widget->width - 2, widget->height - 2, colours->menu);
            if (widget->value[0]) ui_text(canvas, x + 6, y, widget->height, widget->value, colours->text);
            else if (widget->placeholder) ui_text(canvas, x + 6, y, widget->height, widget->placeholder, colours->disabled);
            if (focused) ui_fill(canvas, x + 6 + ui_text_width(widget->value) + 1, y + 5, 1, widget->height - 10, colours->text);
            break;
        case WIDGET_CHOICE:
            ui_fill(canvas, x, y, widget->width, widget->height, border);
            ui_fill(canvas, x + 1, y + 1, widget->width - 2, widget->height - 2, colours->bar);
            if (widget->selected >= 0 && widget->selected < widget->count) ui_text(canvas, x + 6, y, widget->height, widget->options[widget->selected], colours->text);
            ui_mark(canvas, GLYPH_ARROW, x + widget->width - 12, y + (widget->height - MARK_ROW) / 2, MARK_ROW, colours->text);
            break;
        case WIDGET_CHECK:
            ui_fill(canvas, x, y + 4, 16, 16, border);
            ui_fill(canvas, x + 1, y + 5, 14, 14, colours->menu);
            if (widget->checked) ui_mark(canvas, GLYPH_CHECK, x + 8, y + (widget->height - MARK_ROW) / 2, MARK_ROW, colours->text);
            ui_text(canvas, x + 24, y, widget->height, widget->text, colours->text);
            break;
        case WIDGET_BUTTON: {
            bool primary = widget->result == form->default_result;
            ui_fill(canvas, x, y, widget->width, widget->height, focused ? colours->highlight : colours->menu_border);
            ui_fill(canvas, x + 1, y + 1, widget->width - 2, widget->height - 2, primary ? colours->highlight : colours->bar);
            float label_width = ui_text_width(widget->text);
            ui_text(canvas, x + (widget->width - label_width) / 2, y, widget->height, widget->text, primary ? colours->highlight_text : colours->text);
            break;
        }
        }
    }
    if (form->open >= 0) {
        widget_t *widget = &form->widgets[form->open];
        int rows = visible_options(form);
        float x = ox + widget->x, y = oy + widget->y + widget->height;
        ui_fill(canvas, x, y, widget->width, rows * FORM_LIST_ROW + 2, colours->menu_border);
        ui_fill(canvas, x + 1, y + 1, widget->width - 2, rows * FORM_LIST_ROW, colours->menu);
        for (int row = 0; row < rows; row++) {
            int option = form->scroll + row;
            if (option >= widget->count) break;
            bool enabled_option = option_enabled(widget, option);
            bool highlighted = enabled_option && option == form->hover;
            float top = y + 1 + row * FORM_LIST_ROW;
            if (highlighted) ui_fill(canvas, x + 1, top, widget->width - 2, FORM_LIST_ROW, colours->highlight);
            colour_t text = !enabled_option ? colours->disabled : highlighted ? colours->highlight_text : colours->text;
            ui_text(canvas, x + 6, top, FORM_LIST_ROW, widget->options[option], text);
        }
    }
    SDL_RenderPresent(canvas->renderer);
}

static int widget_at(const form_t *form, float x, float y) {
    for (int i = 0; i < form->count; i++) {
        const widget_t *widget = &form->widgets[i];
        float left = form->x + widget->x, top = form->y + widget->y;
        float width = widget->kind == WIDGET_CHECK ? 24 + ui_text_width(widget->text) : widget->width;
        if (ui_inside(x, y, left, top, width, widget->height)) return i;
    }
    return -1;
}

static int option_at(const form_t *form, float x, float y) {
    const widget_t *widget = &form->widgets[form->open];
    float left = form->x + widget->x, top = form->y + widget->y + widget->height + 1;
    int rows = visible_options(form);
    if (!ui_inside(x, y, left, top, widget->width, rows * FORM_LIST_ROW)) return -1;
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
    SDL_StartTextInput(form->window);
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
    SDL_StopTextInput(form->window);
    ui_release_glyphs(&form->canvas);
    return result;
}

#define NEW_CREATE 1

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
    SDL_ShowOpenFileDialog(picked_rom, state, form->window, NULL, 0, NULL, false);
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
        const SDL_MessageBoxData dialog = { SDL_MESSAGEBOX_WARNING, form->window, "Not a Velo ROM", "That file isn't a ROM this emulator can run.", 1, buttons, NULL };
        int chosen;
        SDL_ShowMessageBox(&dialog, &chosen);
    }
    update_screens(form, state);
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    new_machine_t *state = calloc(1, sizeof *state);
    if (!state) return false;
    state->probe = probe;
    state->count = rom_count < FORM_OPTIONS - 1 ? rom_count : FORM_OPTIONS - 1;
    memcpy(state->roms, roms, (size_t)state->count * sizeof roms[0]);
    form_t form = { .title = "New Machine", .message = DIALOG_NEW_MACHINE_MESSAGE, .open = -1, .default_result = NEW_CREATE,
                    .changed = rom_changed, .context = state, .window = window };
    int name = add_widget(&form, WIDGET_TEXT, "Name:");
    form.widgets[name].value = result->name;
    form.widgets[name].value_size = sizeof result->name;
    form.widgets[name].placeholder = "Named from the settings below";
    state->rom = add_widget(&form, WIDGET_CHOICE, "Version:");
    widget_t *rom = &form.widgets[state->rom];
    for (int i = 0; i < state->count; i++) {
        rom->options[i] = state->roms[i].label;
        if (!strcmp(state->roms[i].path, result->rom)) rom->selected = state->last_rom = i;
    }
    rom->options[state->count] = "Other ROM File" ELLIPSIS;
    rom->count = state->count + 1;
    state->screen = add_widget(&form, WIDGET_CHOICE, "Screen:");
    widget_t *screen = &form.widgets[state->screen];
    for (int i = 0; i < SCREEN_PRESET_COUNT && dialog_screen_label(i); i++) {
        screen->options[screen->count++] = dialog_screen_label(i);
        if (SCREEN_PRESETS[i].width == result->screen.width && SCREEN_PRESETS[i].height == result->screen.height) screen->selected = i;
    }
    int memory = add_widget(&form, WIDGET_CHOICE, "Memory:");
    for (int i = 0; i < DIALOG_MEMORY_COUNT; i++) {
        form.widgets[memory].options[form.widgets[memory].count++] = DIALOG_MEMORY_LABELS[i];
        if (DIALOG_MEMORY_SIZES[i] == result->memory) form.widgets[memory].selected = i;
    }
    int clock = add_widget(&form, WIDGET_CHECK, NULL);
    form.widgets[clock].text = DIALOG_CLOCK_LABEL;
    form.widgets[clock].checked = result->host_time;
    int cancel = add_widget(&form, WIDGET_BUTTON, NULL);
    form.widgets[cancel].text = "Cancel";
    form.widgets[cancel].result = FORM_CANCEL;
    int create = add_widget(&form, WIDGET_BUTTON, NULL);
    form.widgets[create].text = "Create";
    form.widgets[create].result = NEW_CREATE;
    update_screens(&form, state);
    bool created = run_form(&form) == NEW_CREATE && rom->selected < state->count;
    if (created) {
        snprintf(result->rom, sizeof result->rom, "%s", state->roms[rom->selected].path);
        result->screen = SCREEN_PRESETS[screen->selected];
        result->memory = DIALOG_MEMORY_SIZES[form.widgets[memory].selected];
        result->host_time = form.widgets[clock].checked;
    }
    free(state);
    return created;
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    char titles[FORM_OPTIONS][120];
    form_t form = { .title = "Manage Machines", .message = DIALOG_MANAGE_MESSAGE, .open = -1, .default_result = DIALOG_MANAGE_CLOSE, .window = window };
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

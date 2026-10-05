#import <Cocoa/Cocoa.h>

#include "app/menu_layout.h"

#define MENU_QUEUE 32

static int queue[MENU_QUEUE];
static int queued = 0;
static NSMenuItem *items[MENU_COUNT];

@interface VeloMenuTarget : NSObject
@end

static VeloMenuTarget *target = nil;

@implementation VeloMenuTarget
- (void)fire:(NSMenuItem *)item {
    if (queued < MENU_QUEUE) queue[queued++] = (int)item.tag;
}
@end

static NSMenuItem *holders[sizeof MENU_ENTRIES / sizeof MENU_ENTRIES[0]];
static int holder_count = 0;

static void add_item(NSMenu *menu, int tag, NSString *title, NSString *key, NSEventModifierFlags modifiers) {
    NSMenuItem *item = [[NSMenuItem alloc] initWithTitle:title action:@selector(fire:) keyEquivalent:key];
    item.keyEquivalentModifierMask = modifiers;
    item.target = target;
    item.tag = tag;
    [menu addItem:item];
    items[tag] = item;
}

static NSMenu *add_menu(NSString *title) {
    NSMenu *menu = [[NSMenu alloc] initWithTitle:title];
    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
    holder.submenu = menu;
    holders[holder_count++] = holder;
    return menu;
}

static void attach_menus(void) {
    NSMenu *bar = [NSApp mainMenu];
    if (!bar) {
        bar = [[NSMenu alloc] init];
        [NSApp setMainMenu:bar];
    }
    for (int i = 0; i < holder_count; i++) {
        if (holders[i].menu == bar) continue;
        if (holders[i].menu) [holders[i].menu removeItem:holders[i]];
        NSInteger window_menu = [bar indexOfItemWithTitle:@"Window"];
        NSInteger position = window_menu >= 0 ? window_menu : bar.numberOfItems;
        [bar insertItem:holders[i] atIndex:position];
    }
}

static NSEventModifierFlags cocoa_modifiers(int modifiers) {
    NSEventModifierFlags flags = 0;
    if (modifiers & MENU_KEY_PRIMARY) flags |= NSEventModifierFlagCommand;
    if (modifiers & MENU_KEY_SHIFT) flags |= NSEventModifierFlagShift;
    if (modifiers & MENU_KEY_CONTROL) flags |= NSEventModifierFlagControl;
    return flags;
}

void menu_install(SDL_Window *window) {
    (void)window;
    target = [[VeloMenuTarget alloc] init];
    NSMenu *stack[4];
    int depth = 0;
    for (int i = 0; i < MENU_ENTRY_COUNT; i++) {
        const menu_entry_t *entry = &MENU_ENTRIES[i];
        NSString *title = entry->title ? [NSString stringWithUTF8String:entry->title] : nil;
        NSMenu *menu = depth ? stack[depth - 1] : nil;
        switch (entry->kind) {
            case MENU_ENTRY_MENU: stack[depth++] = add_menu(title); break;
            case MENU_ENTRY_SUBMENU: {
                NSMenu *submenu = [[NSMenu alloc] initWithTitle:title];
                NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
                holder.submenu = submenu;
                [menu addItem:holder];
                stack[depth++] = submenu;
                break;
            }
            case MENU_ENTRY_END: depth--; break;
            case MENU_ENTRY_SEPARATOR: [menu addItem:[NSMenuItem separatorItem]]; break;
            case MENU_ENTRY_HEADING: {
                NSMenuItem *heading;
                if (@available(macOS 14.0, *)) heading = [NSMenuItem sectionHeaderWithTitle:title];
                else heading = [[NSMenuItem alloc] initWithTitle:title action:nil keyEquivalent:@""];
                heading.enabled = NO;
                [menu addItem:heading];
                break;
            }
            case MENU_ENTRY_ITEM: {
                NSString *key = entry->key ? [NSString stringWithFormat:@"%c", entry->key] : @"";
                add_item(menu, entry->tag, title, key, cocoa_modifiers(entry->modifiers));
                break;
            }
        }
    }
    attach_menus();
}

int menu_bar_height(void) {
    return 0;
}

void menu_insets(int *left, int *top, int *right, int *bottom) {
    *left = *right = *bottom = 0;
    *top = menu_bar_height();
}

bool menu_event(const SDL_Event *event) {
    (void)event;
    return false;
}

bool menu_active(void) {
    return false;
}

void menu_draw(SDL_Renderer *renderer) {
    (void)renderer;
}

void menu_ensure(void) {
    if (holder_count) attach_menus();
}

int menu_poll(void) {
    if (queued == 0) return -1;
    int item = queue[0];
    for (int i = 1; i < queued; i++) queue[i - 1] = queue[i];
    queued--;
    return item;
}

void menu_set_enabled(int item, bool enabled) {
    if (item < 0 || item >= MENU_COUNT || !items[item]) return;
    items[item].target = enabled ? target : nil;
    items[item].action = enabled ? @selector(fire:) : nil;
}

void menu_set_checked(int item, bool checked) {
    if (item < 0 || item >= MENU_COUNT || !items[item]) return;
    NSControlStateValue state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    if (items[item].state != state) items[item].state = state;
}

void menu_set_title(int item, const char *title) {
    if (item < 0 || item >= MENU_COUNT || !items[item]) return;
    NSString *text = [NSString stringWithUTF8String:title];
    if (![items[item].title isEqualToString:text]) items[item].title = text;
}

void menu_set_hidden(int item, bool hidden) {
    if (item < 0 || item >= MENU_COUNT || !items[item]) return;
    if (items[item].hidden != hidden) items[item].hidden = hidden;
}

int menu_modifiers(void) {
    NSEventModifierFlags flags = [NSEvent modifierFlags];
    int modifiers = MENU_MOD_KNOWN;
    if (flags & NSEventModifierFlagShift) modifiers |= MENU_MOD_SHIFT;
    if (flags & NSEventModifierFlagControl) modifiers |= MENU_MOD_CONTROL;
    if (flags & NSEventModifierFlagOption) modifiers |= MENU_MOD_ALT;
    return modifiers;
}

#import <Cocoa/Cocoa.h>

#include "frontend/common/dialog.h"

#include <string.h>

@interface MachineForm : NSObject
@property(strong) NSAlert *alert;
@property(strong) NSTextField *name;
@property(strong) NSPopUpButton *memory;
@property(strong) NSButton *clock;
@property(strong) NSPopUpButton *rom;
@property(strong) NSPopUpButton *screen;
@property(strong) NSMutableArray<NSString *> *paths;
@property(strong) NSMutableArray<NSNumber *> *screens;
@property(assign) dialog_probe_fn probe;
@property(assign) NSInteger lastRom;
@end

@implementation MachineForm

- (void)updateScreens {
    NSInteger index = self.rom.indexOfSelectedItem;
    uint32_t mask = index >= 0 && index < (NSInteger)self.screens.count ? self.screens[(NSUInteger)index].unsignedIntValue : 1u;
    for (NSInteger i = 0; i < self.screen.numberOfItems; i++) [self.screen itemAtIndex:i].enabled = (mask >> i) & 1u;
    if (!self.screen.selectedItem.enabled) [self.screen selectItemAtIndex:0];
}

- (void)romChosen:(id)sender {
    NSInteger index = self.rom.indexOfSelectedItem;
    if (index < (NSInteger)self.paths.count) {
        self.lastRom = index;
        [self updateScreens];
        return;
    }
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.message = [NSString stringWithUTF8String:DIALOG_ROM_PROMPT];
    if ([panel runModal] == NSModalResponseOK && panel.URL) {
        const char *path = panel.URL.fileSystemRepresentation;
        char label[160];
        uint32_t mask = self.probe(path, label, sizeof label);
        if (mask) {
            NSUInteger position = self.paths.count;
            [self.paths addObject:[NSString stringWithUTF8String:path]];
            [self.screens addObject:@(mask)];
            [self.rom insertItemWithTitle:[NSString stringWithUTF8String:label] atIndex:(NSInteger)position];
            [self.rom selectItemAtIndex:(NSInteger)position];
            self.lastRom = (NSInteger)position;
            [self updateScreens];
            return;
        }
        NSAlert *alert = [[NSAlert alloc] init];
        alert.messageText = [NSString stringWithUTF8String:DIALOG_NOT_A_ROM];
        alert.informativeText = [NSString stringWithFormat:@"%@ isn't a ROM this emulator can run.", panel.URL.lastPathComponent];
        [alert runModal];
    }
    if (self.lastRom >= 0) [self.rom selectItemAtIndex:self.lastRom];
    [self updateScreens];
}

@end

static NSTextField *label(NSString *text) {
    NSTextField *field = [NSTextField labelWithString:text];
    field.alignment = NSTextAlignmentRight;
    return field;
}

static NSPopUpButton *popup(void) {
    NSPopUpButton *button = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(0, 0, 320, 26) pullsDown:NO];
    button.autoenablesItems = NO;
    return button;
}

static MachineForm *new_machine_form(const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, const dialog_machine_t *result) {
    MachineForm *form = [[MachineForm alloc] init];
    form.probe = probe;
    form.paths = [NSMutableArray array];
    form.screens = [NSMutableArray array];
    form.lastRom = -1;

    NSTextField *name = [NSTextField textFieldWithString:[NSString stringWithUTF8String:result->name]];
    name.placeholderString = @"Named from the settings below";
    [name.widthAnchor constraintEqualToConstant:320].active = YES;

    form.rom = popup();
    NSInteger selected = -1;
    for (int i = 0; i < rom_count; i++) {
        [form.rom addItemWithTitle:[NSString stringWithUTF8String:roms[i].label]];
        [form.paths addObject:[NSString stringWithUTF8String:roms[i].path]];
        [form.screens addObject:@(roms[i].screens)];
        if (selected < 0 && !strcmp(roms[i].path, result->rom)) selected = i;
    }
    if (rom_count) [form.rom.menu addItem:[NSMenuItem separatorItem]];
    [form.rom addItemWithTitle:@"Other ROM File\u2026"];
    form.rom.target = form;
    form.rom.action = @selector(romChosen:);
    if (rom_count) {
        form.lastRom = selected >= 0 ? selected : 0;
        [form.rom selectItemAtIndex:form.lastRom];
    }

    form.screen = popup();
    for (int i = 0; i < SCREEN_PRESET_COUNT && dialog_screen_label(i); i++) {
        [form.screen addItemWithTitle:[NSString stringWithUTF8String:dialog_screen_label(i)]];
        if (SCREEN_PRESETS[i].width == result->screen.width && SCREEN_PRESETS[i].height == result->screen.height) [form.screen selectItemAtIndex:i];
    }
    [form updateScreens];

    NSPopUpButton *memory = popup();
    for (int i = 0; i < DIALOG_MEMORY_COUNT; i++) {
        [memory addItemWithTitle:[NSString stringWithUTF8String:DIALOG_MEMORY_LABELS[i]]];
        if (DIALOG_MEMORY_SIZES[i] == result->memory) [memory selectItemAtIndex:i];
    }

    NSButton *clock = [NSButton checkboxWithTitle:[NSString stringWithUTF8String:DIALOG_CLOCK_LABEL] target:nil action:nil];
    clock.state = result->host_time ? NSControlStateValueOn : NSControlStateValueOff;

    NSGridView *grid = [NSGridView gridViewWithViews:@[
        @[ label(@"Name:"), name ],
        @[ label(@"Version:"), form.rom ],
        @[ label(@"Screen:"), form.screen ],
        @[ label(@"Memory:"), memory ],
        @[ [NSGridCell emptyContentView], clock ],
    ]];
    grid.rowSpacing = 8;
    grid.columnSpacing = 8;
    [grid columnAtIndex:0].xPlacement = NSGridCellPlacementTrailing;
    for (NSInteger row = 0; row < grid.numberOfRows; row++) [grid rowAtIndex:row].yPlacement = NSGridCellPlacementCenter;
    grid.frame = NSMakeRect(0, 0, grid.fittingSize.width, grid.fittingSize.height);

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"New Machine";
    alert.informativeText = [NSString stringWithUTF8String:DIALOG_NEW_MACHINE_MESSAGE];
    alert.accessoryView = grid;
    [alert addButtonWithTitle:@"Create"];
    [alert addButtonWithTitle:@"Cancel"];
    alert.window.initialFirstResponder = name;
    form.alert = alert;
    form.name = name;
    form.memory = memory;
    form.clock = clock;
    return form;
}

bool dialog_new_machine(SDL_Window *window, const dialog_rom_t *roms, int rom_count, dialog_probe_fn probe, dialog_machine_t *result) {
    (void)window;
    @autoreleasepool {
        MachineForm *form = new_machine_form(roms, rom_count, probe, result);
        NSTextField *name = form.name;
        NSPopUpButton *memory = form.memory;
        NSButton *clock = form.clock;
        if ([form.alert runModal] != NSAlertFirstButtonReturn) return false;

        NSInteger rom = form.rom.indexOfSelectedItem;
        if (rom < 0 || rom >= (NSInteger)form.paths.count) {
            NSAlert *missing = [[NSAlert alloc] init];
            missing.messageText = @"No ROM chosen";
            missing.informativeText = [NSString stringWithUTF8String:DIALOG_NO_ROM_HINT];
            [missing runModal];
            return false;
        }
        snprintf(result->name, sizeof result->name, "%s", name.stringValue.UTF8String);
        snprintf(result->rom, sizeof result->rom, "%s", form.paths[(NSUInteger)rom].UTF8String);
        NSInteger screen = form.screen.indexOfSelectedItem;
        result->screen = SCREEN_PRESETS[screen >= 0 && screen < SCREEN_PRESET_COUNT ? screen : 0];
        NSInteger size = memory.indexOfSelectedItem;
        result->memory = DIALOG_MEMORY_SIZES[size >= 0 && size < DIALOG_MEMORY_COUNT ? size : 0];
        result->host_time = clock.state == NSControlStateValueOn;
        return true;
    }
}

static NSAlert *manage_alert(const char *const *names, int count, int current, int chosen, NSPopUpButton **result) {
    NSPopUpButton *list = popup();
    for (int i = 0; i < count; i++) {
        NSString *title = [NSString stringWithUTF8String:names[i]];
        if (i == current) title = [title stringByAppendingString:@" (running)"];
        [list addItemWithTitle:title];
    }
    if (chosen >= 0 && chosen < count) [list selectItemAtIndex:chosen];

    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"Manage Machines";
    alert.informativeText = [NSString stringWithUTF8String:DIALOG_MANAGE_MESSAGE];
    alert.accessoryView = list;
    [alert addButtonWithTitle:@"Done"];
    [alert addButtonWithTitle:@"Reset\u2026"];
    [alert addButtonWithTitle:@"Delete\u2026"];
    *result = list;
    return alert;
}

dialog_manage_t dialog_manage_machines(SDL_Window *window, const char *const *names, int count, int current, int *chosen) {
    (void)window;
    @autoreleasepool {
        NSPopUpButton *list;
        NSAlert *alert = manage_alert(names, count, current, *chosen, &list);
        NSModalResponse response = [alert runModal];
        *chosen = (int)list.indexOfSelectedItem;
        if (response == NSAlertSecondButtonReturn) return DIALOG_MANAGE_RESET;
        if (response == NSAlertThirdButtonReturn) return DIALOG_MANAGE_DELETE;
        return DIALOG_MANAGE_CLOSE;
    }
}

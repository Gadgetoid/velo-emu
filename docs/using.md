# Using the emulator

## ROMs

No ROMs are included. Put them in the `roms` folder of the [data folder](#data-folder-and-settings), under any names.

| System | ROM |
| --- | --- |
| Windows CE 1.0 | The 7,799,876-byte `nk.bin` from CERF's `philips_velo_1_ce1` bundle |
| Windows CE 2.0, merged | An image with the upgrade's applications in ROM too, which leaves the PC Card slot free for apps and games |
| Windows CE 2.0, upgrade ROM only | The upgrade's ROM on its own, from CERF's `philips_velo_1_ce2` bundle (4,185,248 bytes), with the applications on a CompactFlash card image (see [PC Card storage](pc-card.md)) |

The Velo's CE 2.0 upgrade was a ROM Miniature Card, with its applications (Pocket Word, Pocket Excel and the rest) on a CompactFlash card in the PC Card slot.

Each file is identified by its contents. With more than one image for a system:

1. One built by velo-emu-ce-2.0 with its patch sets is used first. For CE 2.0, one with `pc-link-115k` comes ahead of one without. Its `\Windows\velo-emu-ce-2.0.txt` lists the sets and the system, and only counts when that system matches the ROM's own.
2. Then the larger image, so a merged image wins over the upgrade's ROM on its own.

New Machine marks the patched images. With no ROMs in the folder, a dialog shows its location, with a button to open it.

## Running

```
velo
velo ~/roms/nk.bin
velo --memory=32 --serial=net --card=apps.img nk-ce2-merged.bin
velo --help
```

- Open the app, or run `velo`. It starts the last machine used.
- Each machine is a ROM plus fixed hardware (screen size and memory) with its own saved state. The machines are listed in the Machine menu, which switches between them.
- The first launch makes a Windows CE 1.0 and a Windows CE 2.0 machine from the ROMs in the roms folder, keeping any existing saved states.
- `--machine=NAME` opens a machine by name.
- A ROM on the command line runs that ROM instead, with its own saved state outside the machine list.

### First boot

A first boot goes through the setup wizard: touch calibration, time zone, date and owner.

- The mouse is the stylus. Hold it on each calibration target for about half a second.
- With "Set the clock from this computer" (on by default when making a machine) the clock starts at the host's time, so pick your home city in the wizard and it's right.

## Data folder and settings

| | macOS | Linux |
| --- | --- | --- |
| Data folder: ROMs, machines, states, snapshots, cards | `~/Library/Application Support/Velo` | `~/.local/share/velo-emu` |
| Settings, `emu.ini` | The data folder | `~/.config/velo-emu` |

- The data folder also holds `rapi.sock`, `debug.log` and the shared folder's sync manifest.
- `XDG_DATA_HOME` and `XDG_CONFIG_HOME` override the locations, as the tests do.
- On macOS the first launch moves an older `~/.local/share/velo-emu` and `~/.config/velo-emu/emu.ini` into `~/Library/Application Support/Velo`.

## Controls

- The mouse is the stylus. Host keys map to the Velo keyboard.
- CE 1.0 and 2.0 have no scroll wheel, so scrolling (a mouse wheel or two-finger scroll) presses the arrow keys instead: it scrolls Pocket IE and lists, and moves the caret in documents.
- Dropping files on the window sends them to `\My Documents`. A dropped `.load` script installs its package ([installing software](installing-software.md)), and a single dropped `.img` is inserted as the card.
- The backlight is under CE's control: the backlight key toggles it, and the Backlight control panel's idle timeout turns it off (30 seconds by default, since the Velo reports external power).

## Menus

On macOS the menus are in the menu bar. On Linux they're in a bar along the top of the window: F10 opens it, and the arrow keys, Enter and Escape move through it.

| Menu | Item | Shortcut |
|---|---|---|
| Machine | Power Button (suspend and resume) | Cmd-Shift-P |
| Machine | Backlight (presses the Velo's backlight key) | Cmd-B |
| Machine | Soft Reset (restarts CE, keeping RAM and the object store, like the reset button) | Cmd-R |
| Machine | Machines: the machine list (switching saves the current machine and opens the other), New Machine, Manage Machines (Reset back to the factory state, Delete) | |
| Machine | Pause | Cmd-P |
| Machine | CPU Speed: 1x (original), 2x, 4x, 8x, and [Optimisations](optimisations.md) | |
| Machine | Show Debug Output | |
| State | Save State, Load State | Cmd-S, Cmd-L |
| State | Save Snapshot, Load Snapshot | Cmd-Ctrl-S, Cmd-Ctrl-L |
| State | Show State Folder | |
| Edit | Copy Screen (a PNG of the screen as shown) | Cmd-C |
| Edit | Paste as Typing (types the clipboard; curly quotes and dashes become plain ones, other characters are skipped) | Cmd-V |
| Edit | Save Screenshot to Desktop | Cmd-Shift-S |
| View | 50%, 75%, Actual Size, 150%, 200%; Zoom In and Zoom Out | Cmd-0, Cmd-=, Cmd-- |
| View | Simulated LCD (glass, ghosting and backlight) or Sharp Pixels (plain greys, one per screen pixel) | |
| View | Full Screen | Cmd-Ctrl-F |
| Devices | PC Card: Insert Card Image, Eject Card | Cmd-O, Cmd-E |
| Devices | Paravirtual Disk: Insert Disk Image, New Disk Image, Eject Disk (needs the guest driver) | |
| Devices | Serial Port: Not Connected, Network (PPP), Pseudo-terminal, TCP Port, Host Serial Port (the detected ports) | Cmd-Shift-N for Network |
| Devices | Connect Network at Launch | |
| Devices | Sound | |
| PC Link | Send Files to Velo, Copy My Documents to Mac | |
| PC Link | Shared Folder, Sync Shared Folder Now, Stop Sharing Folder | |
| PC Link | Velo Settings: Set Up Pocket IE Proxy, Connection Speed (19200 (original), 38400, 57600, 115200) | |

### Shortcuts on Linux

- Shortcuts use Cmd on macOS and Ctrl+Alt on Linux, so Cmd-Shift-P is Shift+Ctrl+Alt+P. Plain Ctrl and Alt go to the Velo.
- Full Screen is F11, and the snapshot items have no shortcut.
- Desktops that lock the screen with Ctrl+Alt+L take that one from Load State.
- Linux menus say Computer for Mac and Pictures for Desktop, where screenshots go.
- Machine has a Quit item (Ctrl+Alt+Q).

## Saved state and snapshots

Each ROM has its own saved machine, `state-ROM-HASH.bin` in the data folder.

- It's saved on quit, every minute and by Save State, and restored on launch with the clock advanced by the time away.
- `--fresh` cold boots instead, and `--state=FILE` uses FILE for loading, saving and autosaving.
- Load State returns to the last save.
- A state only loads with the ROM it was made with.
- States from older builds load, with any new fields at their defaults. One that can't be read is renamed with `.old` appended.
- States are saved gzip-compressed (a 33 MB machine saves as about 2 MB). Uncompressed states from older builds still load, but older builds can't read compressed ones.

### Snapshots

Snapshots are named copies of the machine, in `snapshots` in the data folder by default. Loading one is a restore point: autosave carries on to the ROM's own state.

### Backups

Backups go in `snapshots/Backups`:

- a copy of the machine every 10 minutes it runs
- one just before a reset, Load State and Load Snapshot replace it
- before a machine is deleted
- before `--fresh` starts over the saved state

The newest 10 per state are kept. Load one with State > Load Snapshot.

### Restoring connections and cards

- If a serial cable was connected when the state was saved, the restored machine starts with it unplugged and plugs it back in two seconds later, so CE dials again instead of reusing a PPP session that no longer exists.
- If the state's card image has gone, the card starts out ejected and a newly inserted one goes in a second later, so CE registers the removal first.

### Reading a state with velo-state

`velo-state` reads a saved state's RAM files and registry without running it, for states that won't boot, or to compare two:

```
velo-state state.bin ls "/Program Files"
velo-state state.bin get Samples/Letter.pwd
velo-state state.bin reg dump HKLM/Drivers
velo-state before.bin diff after.bin
```

- Paths, keys and output follow `velo-rapi`.
- Files in ROM aren't listed, and databases aren't read.
- On CE 1.0 the registry comes from `\Windows\Pegreg.reg`, as filesys last wrote it.
- The store's format is in [object-store.md](object-store.md).

## Machines

Machine > New Machine picks:

| Setting | Notes |
| --- | --- |
| Name | Left blank, it's made from the other settings |
| ROM | From the roms folder, or Other ROM File for any other, such as a patched or merged image |
| Screen size | See [Screen](#screen) |
| Memory | See [Memory](#memory) |
| Set the clock from the host | See [Clock](#clock) |

- These are fixed for the life of the machine.
- Machine > Manage Machines resets a machine to its factory state or deletes it. Both back up its state first, and the running machine can't be deleted.
- Machines are kept as `machines/*.ini` in the data folder, each with its saved state beside it.
- Switching or quitting saves the machine.

## Clock

A machine made with the clock set from the host gets the host's time at a cold boot.

- CE starts at noon on 1 January (1996 for CE 1.0, 1997 for CE 2.0) in its default time zone, Pacific.
- With the option on, the emulator gives it the host's time in Pacific time, so once you pick your home city the clock is right.
- After that the clock keeps running while the emulator is closed, and survives a soft reset.

## Memory

Memory is the machine's RAM, or `--memory=` for a ROM given on the command line.

| Memory | Notes |
| --- | --- |
| 4, 8, 16 MB | Built-in RAM. CE uses at most 16 MB of it. |
| 20 MB, 32 MB | Add a 16 MB DRAM Miniature Card, the Velo's own memory expansion, which CE maps as a second RAM region (20,348 KB and 32,636 KB in Control Panel > System) |

- CE 2.0 needs 12 MB, so give it 20 or 32. CE 2.0 machines get 32 MB unless New Machine says otherwise.
- A saved machine keeps the memory it was booted with.

## Screen

Screen is the machine's display size, or `--screen=WxH` for a ROM given on the command line: 480x240, 640x240, 640x480 or 800x600.

| ROM | Sizes |
| --- | --- |
| CE 1.0 | Any |
| Merged CE 2.0 image | Up to 640 x 480 |
| CE 2.0 upgrade ROM on its own | 640 x 240, as its display driver faults at taller sizes |

- The emulator patches the display setup in the loaded ROM (the kernel's LCD controller setup and GWES's or the display driver's size, stride and framebuffer), keeping the original refresh rate, and moves the framebuffer out of the way of the larger image.
- Sizes a ROM can't run are greyed out in New Machine.
- The ROM files aren't changed, and a saved machine keeps the screen it was booted with.
- Some of CE's own dialogs, such as the setup wizard's, keep their 480 x 240 layout.

## Speed

CPU Speed runs that many instructions per 36.864 MHz clock tick. `--speed=` does the same. Timers, the RTC, the LCD, sound and serial stay on the real clock, so only the CPU gets faster.

CPU Speed > Optimisations does some of CE's work natively. See [optimisations.md](optimisations.md).

# velo-emu

An emulator for the Philips Velo 1 (1997), a Windows CE Handheld PC. It runs the stock CE 1.0 ROM and the CE 2.0 upgrade, with a simulated backlit LCD, PC Card images, a PPP network with a web proxy for Pocket IE, and the desktop connection tools of the day. Unlike [CERF](https://github.com/gweslab/cerf), which it draws on, it runs on macOS, Linux and Android.

![Windows CE 1.0 desktop](docs/screenshots/ce1-desktop.png)
![Windows CE 2.0 desktop](docs/screenshots/ce2-desktop.png)
![Pocket IE 2.0 browsing through the proxy](docs/screenshots/ce2-pocket-ie.png)

## Install

- **macOS (Apple silicon):** `Velo.app` from a [release](https://github.com/Gadgetoid/velo-emu/releases). It isn't notarised: allow it in System Settings > Privacy & Security > Open Anyway, or run `xattr -dr com.apple.quarantine Velo.app`. `velo-rapi` and `velo-state` are inside it, in `Velo.app/Contents/MacOS`.
- **Debian 12 or later, Ubuntu 24.04 or later:** the `.deb` from a release. It installs `velo`, `velo-headless`, `velo-rapi` and `velo-state`, with a desktop entry.
- **Android (arm64, Android 9 or later):** the `.apk` from a release. See [docs/android.md](docs/android.md).

To build it yourself, see [docs/building.md](docs/building.md).

## Getting started

No ROMs are included. You need the CE 1.0 `nk.bin` from CERF's `philips_velo_1_ce1` bundle, and for CE 2.0, the upgrade's ROM from `philips_velo_1_ce2` or a merged image ([which ROMs](docs/using.md#roms)).

1. Put the ROMs in the `roms` folder of the data folder, under any names: `~/Library/Application Support/Velo/roms` on macOS, `~/.local/share/velo-emu/roms` on Linux. On Android the app asks for them on its first run.
2. Start `velo`. The first launch makes a Windows CE 1.0 and a Windows CE 2.0 machine from the ROMs it finds. Machine > New Machine makes more later.
3. A first boot goes through the setup wizard: touch calibration, time zone, date and owner. Hold the mouse on each calibration target for about half a second, and pick your home city so the clock is right.

The mouse is the stylus, host keys map to the Velo keyboard and scrolling presses the arrow keys. Menu shortcuts use Cmd on macOS and Ctrl+Alt on Linux, where F10 opens the menu bar. The machine is saved on quit and every minute.

## Command-line options

`velo [OPTIONS] [ROM]`. With no ROM it opens the last machine used. `--help` lists every option.

| Option | What it does |
| --- | --- |
| `--machine=NAME` | Open a machine by name |
| `--fresh` | Ignore the saved state and cold boot |
| `--state=FILE` | Load, save and autosave FILE instead of the ROM's own state ([saved state](docs/using.md#saved-state-and-snapshots)) |
| `--card=IMAGE` | Insert a PC Card image ([PC Card storage](docs/pc-card.md)) |
| `--disk=IMAGE` | Attach a disk image to the [paravirtual disk](docs/paravirtual-disk.md) |
| `--memory=MB` | RAM for a ROM given on the command line: 4, 8, 16, 20 or 32 ([memory](docs/using.md#memory)) |
| `--screen=WxH` | Screen for a ROM given on the command line ([screen](docs/using.md#screen)) |
| `--serial=net\|pty\|tcp[:PORT]\|off\|PORT` | Connect COM1 to the PPP network, a pseudo-terminal, a TCP port or a host serial port ([networking](docs/networking.md)) |
| `--speed=N` | CPU speed multiple: 1, 2, 4 or 8 |
| `--optimisations=on\|off` | Run some of CE's work natively ([optimisations](docs/optimisations.md)) |
| `--gdb=PORT` | Listen for GDB ([debugging](docs/debugging.md)) |

## Documentation

- [Using the emulator](docs/using.md): ROMs, machines, menus, the data folder, saved states, clock, memory, screen and speed
- [PC Card storage](docs/pc-card.md): making card images and the Velo Software Library card
- [Paravirtual disk](docs/paravirtual-disk.md): an experimental swappable disk, with its guest driver
- [Networking and PC Link](docs/networking.md): PPP, serial ports, RAPI, `velo-rapi`, the shared folder and the web proxy
- [Installing CE 1.0 software](docs/installing-software.md): `.load` scripts, with Power Toys as an example
- [Headless](docs/headless.md): running without a window, for tests and scripts
- [Debugging](docs/debugging.md): debug output, GDB and the host mailbox
- [Optimisations](docs/optimisations.md)
- [Android](docs/android.md)
- [Building](docs/building.md)
- [Tests](docs/testing.md)
- [Hardware notes](docs/hardware.md): what's emulated and Windows CE 2.0 details
- [Object store](docs/object-store.md): the format of CE's RAM object store

## Licence

MIT, see `LICENSE`. ROMs and Windows CE software are not included.

## Credits

Peripheral behaviour, the memory map and the keyboard table follow [CERF](https://github.com/gweslab/cerf) (MIT, `licences/MIT-CERF.txt`). The web proxy decodes images with [stb_image](https://github.com/nothings/stb) (public domain) and [nanosvg](https://github.com/memononen/nanosvg) (zlib, `licences/Zlib-nanosvg.txt`), which also draws the icon from `assets/velo.svg`. The Linux menu bar draws text with [stb_truetype](https://github.com/nothings/stb) (public domain) and the system's sans-serif font.

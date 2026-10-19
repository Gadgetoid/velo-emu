# PC Card storage

The PC Card slot takes a CompactFlash (ATA) card backed by a raw disk image. CE 1.0 mounts it as `\PC Card` and CE 2.0 as `\Storage Card`.

![A software library running from the PC Card](screenshots/ce2-library.png)
![Start > Programs > Games with the Entertainment Pack](screenshots/ce2-games-menu.png)
![Chess from the Entertainment Pack](screenshots/ce2-chess.png)

## Requirements

`tools/mkcard.sh` makes card images. It needs mtools:

| Platform | Install |
| --- | --- |
| macOS | `brew install mtools` (it also uses `hdiutil`, which is built in) |
| Debian or Ubuntu | `apt install mtools` (it also uses `sfdisk` and `mkfs.fat`) |

## Making a card

Make one, optionally copying folders onto it:

```
tools/mkcard.sh card.img 32 ~/Downloads/SOFTWARE
velo --card=card.img
```

## Inserting and ejecting

| Where | How |
| --- | --- |
| Command line | `--card=IMAGE` |
| Devices menu | Insert Card Image (Cmd-O), Eject Card (Cmd-E) |
| Drag and drop | Drop a single `.img` on the window |

- The image path is kept in the saved state.
- Inserting over a card ejects the old one and inserts the new one a second later.

## Changing an image on the host

Eject the card first, then:

| Platform | Command |
| --- | --- |
| macOS | `hdiutil attach -imagekey diskimage-class=CRawDiskImage card.img` mounts it |
| Linux | `mcopy -i card.img@@512` copies to and from it |

## The Velo Software Library card

A new machine's first boot inserts the Velo Software Library card from the `cards` folder in the data folder:

| macOS | Linux |
| --- | --- |
| `~/Library/Application Support/Velo/cards` | `~/.local/share/velo-emu/cards` |

It picks the one with a `VELOLIB` folder for CE 1.0, or `VELOLIB2` for CE 2.0. After that the machine keeps whatever card it has.

## How images are made

- `mkcard.sh` formats with `hdiutil` on macOS or `sfdisk` and `mkfs.fat` on Linux, and copies with `mcopy`.
- `tools/fatcase.py` then gives every short lower-case name a long-name entry, since CE ignores the NT case flags mtools and macOS use for them.

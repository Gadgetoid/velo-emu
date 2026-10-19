# Paravirtual disk (experimental)

A disk separate from the PC Card slot, backed by an image file you can swap while the Velo runs. It's emulator-only hardware (a few registers and a sector buffer at physical `0x10800000`) with a small driver, `guest/vdisk`, that registers a disk with CE's FATFS when an image is inserted and removes it when it's ejected.

| System | Mounts it as |
| --- | --- |
| CE 2.0 | A storage card folder alongside a PC Card: whichever mounts first is `\Storage Card`, the other `\Storage Card2` |
| CE 1.0 | `\PC Card`. CE 1.0 only mounts one FAT volume, so use either the paravirtual disk or a PC Card storage card, not both. |

## Requirements

- velo-toolchain, to build the driver. `make guest` looks for it in `../velo-toolchain`; set `VELO_TOOLCHAIN=PATH` otherwise.
- A machine connected over PC Link, to install the driver with `velo-rapi` (see [networking.md](networking.md)). The CE 2.0 upgrade ROM on its own has no RAPI server; tested on CE 1.0 and the merged CE 2.0 image.

The driver is one of the Guest Additions in `guest/` (see `guest/README.md`), which velo-emu-ce-2.0 can also build into a card or ROM.

## Installing the driver

1. `make guest`. The driver lands in `build/guest/vdisk/ce1` and `ce2`.
2. Install the one for the system once, over RAPI with the machine connected:

   ```
   velo-rapi put build/guest/vdisk/ce2/vdisk.dll /Windows/vdisk.dll
   velo-rapi reg set HKLM/Drivers/BuiltIn/VDisk Dll string vdisk.dll
   velo-rapi reg set HKLM/Drivers/BuiltIn/VDisk Entry string VDiskStart
   velo-rapi reg set HKLM/Drivers/BuiltIn/VDisk Keep dword 1
   velo-rapi reg set HKLM/Drivers/BuiltIn/VDisk Order dword 3
   ```

3. Soft reset.

## Using it

| Where | How |
| --- | --- |
| Devices > Paravirtual Disk | Insert Disk Image, New Disk Image (a blank 32 MB image, which the Velo offers to format), Eject Disk |
| Command line | `--disk=IMAGE` at launch |
| headless | `--disk=IMAGE`, `--insert-disk=SECONDS:IMAGE`, `--eject-disk=SECONDS` |

- Any `mkcard.sh` image works (see [PC Card storage](pc-card.md)).
- The driver checks for changes twice a second and mounts once the shell is up.
- The image path is kept in the saved state.

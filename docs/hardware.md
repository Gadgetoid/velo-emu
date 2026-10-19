# Hardware notes

## Emulated

- CPU: MIPS-I interpreter with the TX39 CP0, 32-entry TLB and branch-likely instructions (`src/core/mips.c`).
- PR31500 (`src/core/machine.c`):
  - interrupt controller, including the high-priority encoder
  - periodic timer, RTC and alarm
  - power, with STOPCPU idle, the stop timer and suspend (clock stop, resume in place, woken by the power button or an enabled interrupt)
  - LCD controller (2 and 4 bpp, VIDEO_CTL7 shade map), with the frame and DF interrupts and the power controller's VIDRF video clock divider
  - SPI
  - SIB subframe 0 and sound transmit DMA (half and end interrupts, 16-bit high byte first)
  - I/O and MFIO
- Velo 1 board:
  - 4 to 16 MB DRAM, and the 16 MB DRAM Miniature Card in slot 1 with its I2C ID EEPROM (MFIO 18/20)
  - keyboard controller enable packet and scancodes
  - UCB1100 touch and battery ADC
  - debug module probe
  - M-Module (IT8368) ID
  - LCD panel power on MFIO 17 (active low) and the backlight on MFIO 25
  - M-Module IT8368E PC Card socket (card detect, power, reset, interrupt to the IR block's CARDET) and the PR31500 card windows
  - CompactFlash card in ATA mode (CIS, task file, PIO read and write)
  - UART A (COM1) with its circular receive DMA, CTS on MFIO 30 and DCD on IO 4 (active low)

Guest time is the instruction count at 36.864 MHz. Registers that aren't modelled read back the last value written; `--verbose` logs accesses to them.

## Not emulated

- Sound input
- UART B
- IrDA
- PC Cards other than CompactFlash
- Miniature Cards other than the DRAM card and the CE 2.0 ROM card

## Windows CE 2.0

The Velo 1's CE 2.0 upgrade shipped as a ROM Miniature Card.

- An `nk.bin` whose single ROM header spans the whole file is mapped at the header's `physfirst` (0x90001000, the card window at physical 0x10000000) and started there, as the Velo's boot block would hand off to the card.
- The first 4 KB of the card, missing from the dump, reads as erased flash.
- A B000FF image also loads, with records in the card window and the internal ROM window at 0x1F400000.
- CE 2.0 runs the LCD in 16 greys.

## Internal ROM and reset

The Velo 1's internal ROM is an 8 MB chip, at 0x1F400000 and mirrored every 8 MB from 0x1F000000 to 0x1FFFFFFF, with zeros after the CE 1.0 image, so its boot block at 0x1FC00000 is the start of `nk.bin`. Images larger than the chip, such as merged CE 2.0 ones, aren't mirrored.

Reset (Start > Run, `reset`, or after an install) jumps to the boot block, and the emulator does a warm reset there: CE restarts from the ROM's entry with RAM kept, so it keeps its object store and loads newly installed drivers.

## CE 1.0 FAT write fix

In CE 1.0's `fatfs.dll`, the function that sizes a direct multi-sector write computes the bytes left in a contiguous cluster run as `run_end - (pos - run_start)` instead of `run_end - pos`, so a large write into a fragmented card runs over other files. The emulator patches that instruction in the loaded ROM (0x9F5B4FCC), not the file, and only if the original is there.

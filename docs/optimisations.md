# Optimisations

CPU Speed > Optimisations, or `--optimisations=on`, does some of CE's work natively and skips work that only waits for time to pass. It's on by default on Android.

## What runs natively

- CE 1.0's LZW and CE 2.0's LZ ROM compression run natively, with the same results as CE's own code. CE uses them to load programs and files from ROM and for its RAM object store.
- On CE 2.0 these also run natively:
  - the kernel's export lookup by name, its `memmove`, page clearing and ASCII to Unicode copy
  - `coredll`'s case-mapping table lookup behind `towlower`/`towupper`
  - the display driver's GPE `EmulatedBlt` for copies and other source ROPs, with or without a palette lookup

  Each is recognised by its code: in the ROM for the kernel, and in memory for `coredll` the first time it's called.
- When CE keeps polling the RTC or `GetTickCount` until the time changes, the CPU waits for the next tick instead. This never skips past an interrupt or input.

With Optimisations off, the emulation matches the original instruction for instruction.

## Checking them

| Command | What it does |
| --- | --- |
| `make test` | Runs `build/native-check ROM`, which boots the ROM for 20 seconds, then runs each hooked function as CE's code on a bare CPU and natively, on random inputs, and compares the results |
| `headless --verify-optimisations` | Runs CE's own code for every hooked call while running the native version against a copy of the memory it writes, and logs any call where the two differ when CE's returns |

## Source

- The native routines are in `src/native`, shared with sh3-emu, and don't depend on the CPU.
- `src/core/optimiser.c` has the MIPS calling convention and the two systems' profiles: where each function is and its code.

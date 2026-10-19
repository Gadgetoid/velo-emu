# Tests

## Requirements

| Command | Needs |
| --- | --- |
| `make check` | No ROMs |
| `make test` | `rom/nk.bin` (CE 1.0), and optionally `rom/nk-ce2.bin` (the CE 2.0 upgrade's ROM on its own) |

## Commands

| Command | What it does |
| --- | --- |
| `make check` | Tests the command lines, and the web proxy's rewriting and image conversion |
| `make test` | Runs the full suite |
| `make test ROM=PATH CE2_ROM=PATH` | Runs it against other ROMs |

## What `make test` covers

- Boots both systems through the wizard to the desktop and compares framebuffer hashes at each step.
- Saved states, suspend and resume, cards, memory sizes and the clock.
- PPP, the web proxy in Pocket IE, RAPI file transfer, sync, registry setup and `.load` scripts.
- Reading saved states with `velo-state`.
- `build/native-check`, which checks the native routines against CE's own (see [optimisations.md](optimisations.md#checking-them)).

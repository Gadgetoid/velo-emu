# Headless

`velo-headless` (`headless` in a source build) runs the machine without a window, for tests and scripts. Input happens at emulated times, and runs are deterministic: the same ROM, state and options give the same screen.

## Usage

```
velo-headless nk.bin --seconds=12 --key=8:4B --tap=12:240:120 --pgm=out.pgm
velo-headless nk.bin --seconds=3 --load=state.bin --save=state.bin
velo-headless nk.bin --seconds=10 --load=state.bin --power=2 --power=6 --wav=out.wav
velo-headless nk-ce2-merged.bin --load=state.bin --tap=4:120:40 --png=screen.png --png-backlight=on
velo-headless --help
```

`--help` lists every option.

## Input

| Option | What it does |
| --- | --- |
| `--tap=SECONDS:X:Y[:HOLD]` | Holds the pen for 0.5 s by default; use 0.08 for double taps |
| `--key` | Takes a Velo scancode in hex (the backlight key is 5E), or up to four joined with `+` to press together, such as `19+11` for Alt-Tab |
| `--type` | Types text, with `\n` for Enter |

## Output

| Option | What it does |
| --- | --- |
| `--pgm=FILE` | Saves the raw greyscale screen |
| `--png=FILE` | Saves it through the simulated LCD, as the GUI draws it |

## Connections

| Option | What it does |
| --- | --- |
| `--net=SECONDS` | Connects the PPP network |
| `--rapi=SOCKET` | Makes the Velo's RAPI port available for `velo-rapi --socket` |
| `--rapi-port=PORT` | Accepts RAPI connections on a TCP port, like PC Link > RAPI over the Network |
| `--tcp=PORT` | Offers COM1 on a TCP port, like Devices > TCP Port |
| `--realtime[=N]` | Paces the run at N times real time, for anything driving it over RAPI (unpaced, an idle Velo runs about 1000 times faster) |
| `--cable`, `--cable-send` | Connect a bare serial cable and send bytes down it |
| `--replug=SECONDS` | Unplugs the `--net` cable and plugs it back in two seconds later |

The paravirtual disk options are in [paravirtual-disk.md](paravirtual-disk.md).

## Debugging

| Option | What it does |
| --- | --- |
| `--watch-pc=VA` | Logs registers each time the CPU reaches an address; below 0x02000000 it matches in any process slot |
| `--debug-output` | Prints CE's debug output (see [Debug output](debugging.md#debug-output)) |
| `--gdb=PORT`, `--gdb-process=NAME` | Wait for GDB before running (see [Debugging with GDB](debugging.md#gdb)) |
| `--agent=SOCKET` | The [host mailbox](debugging.md#host-mailbox) |
| `--verify-optimisations` | See [optimisations.md](optimisations.md#checking-them) |

## Notes

- SIGTERM or SIGINT ends a run early and still writes `--save`, `--pgm`, `--png` and `--wav`.
- Unknown options, malformed values and events past the end of a run are errors or warnings, rather than being ignored.
- All three tools take `--help` and `--version`, and options that take a value accept `--name=VALUE` or `--name VALUE`.

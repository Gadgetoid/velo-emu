# Debugging

## Debug output

CE's debug output, from `OutputDebugString` in programs and the kernel's own messages (its boot banner, and a register dump when a program crashes), goes to `debug.log` in the data folder.

| Where | How |
| --- | --- |
| Machine > Show Debug Output | Opens `debug.log` |
| `velo --debug-output`, `headless --debug-output` | Also print it to stderr |

- The retail ROMs build these messages and then drop them, so the emulator reads each string where the OAL's `OEMWriteDebugString` would have sent it to the debug port.
- CE 1.0 also gates the kernel's messages behind a flag, so they're read where `NKDbgPrintfW` drops them.
- The log is moved to `debug.log.old` at launch once it passes 1 MB.

`--verbose` logs hardware, network and proxy activity to stderr.

## GDB

### Requirements

Any GDB with MIPS support works as the client:

| Platform | GDB |
| --- | --- |
| macOS | Homebrew's `gdb` |
| Linux | `gdb-multiarch` |

### Usage

```
velo-headless nk.bin --load=state.bin --card=card.img --gdb=2159 --gdb-process=maths.exe
gdb maths.elf -ex 'target remote :2159'
```

| Option | What it does |
| --- | --- |
| `headless --gdb=PORT` | Waits for GDB on 127.0.0.1:PORT before running, and then runs until GDB detaches or kills it, or until `--seconds` if that's given |
| `velo --gdb=PORT` | Listens while the Velo runs. GDB can attach and detach at any time. |
| Machine > GDB Server | The same on port 1234 (`gdb_port` in `emu.ini`) on every network interface, and remembered |
| `--gdb-process=NAME`, or `monitor process NAME` in GDB | Picks one process |

Machine > GDB Server lets GDB on another computer reach the emulator, including the Android app over Wi-Fi: `target extended-remote 192.168.1.20:1234`. With velo-toolchain's debugmgr in `\Windows\StartUp` (copy it with `velo-rapi --connect`), `remote put` and `run` work there too.

### Processes and memory

- Breakpoints and watchpoints are kept by the emulator, so guest memory isn't changed.
- Memory reads and writes go through CE's own page tables for the process being debugged.
- Every process runs at the same low addresses, so `--gdb-process=NAME` picks one: a breakpoint below 0x02000000 then only stops there. If the process isn't running yet, GDB stops when it starts.

### Crashes

When a program crashes, GDB stops with SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGTRAP at the faulting instruction, with the registers as they were. CE has already started its own handling by then, so continuing lets CE end the program. CE's debug output appears in GDB while it runs.

### DLLs

DLLs the process loads are reported to GDB, which loads symbols for them from the `.elf` beside each DLL (`set solib-search-path` to the build folder), so breakpoints in a DLL work before it's loaded. `monitor modules` lists the loaded modules with an `add-symbol-file` command for each.

### Monitor commands

`monitor help` lists the rest:

| Command |
| --- |
| `processes` |
| `process` |
| `modules` |
| `libraries elf\|dll` |
| `catch on\|off` |
| `output on\|off` |

This works on CE 1.0 and 2.0. Threads aren't reported to GDB yet; it sees one thread, the one that's running.

### Copying files and starting programs

With velo-toolchain's `debugmgr.exe` running on the Velo (see [Host mailbox](#host-mailbox)), GDB can also copy files and start programs. The stub sends debugmgr requests through the mailbox itself, so it needs no `--agent`, and the two can be used together:

```
gdb maths.elf -ex 'target extended-remote :2159'
(gdb) remote put maths.exe /Windows/maths.exe
(gdb) set remote exec-file /Windows/maths.exe
(gdb) break WinMain
(gdb) run
```

- `target remote` debugs the whole machine as it's running.
- `target extended-remote` starts with no process, as `gdbserver --multi` does, so IDEs such as VS Code's C/C++ extension start the program rather than attach.
- `remote put`, `remote get` and `remote delete` go through debugmgr, and so does reading the program's file when GDB asks for it.
- `run` starts the program, debugs that process and stops when it starts. `kill` ends it, and GDB is told when it exits.
- GDB treats `\` in `remote put` and `remote get` paths as an escape, so use `/` (the stub turns it into `\`) or double it.
- While a request is being handled the Velo runs, so other programs carry on.
- If debugmgr doesn't answer, GDB is told file transfer isn't supported and falls back to local files.

### IDEs

The stub also supports GDB's non-stop protocol, so with `maint set target-non-stop on` GDB can list threads and read memory while the program runs, which IDEs do.

VS Code's C/C++ extension needs `miDebuggerServerAddress` and `"useExtendedRemote": true` in the launch configuration as well, or it treats the program as a local process and its Pause and Stop do nothing.

## Host mailbox

An emulator-only message pipe between a program running on the Velo and a tool on the host, for agents such as velo-toolchain's debug manager that transfer and launch programs faster than RAPI. The emulator only passes messages; what they mean is up to the agent and its host tool.

### In the guest

`break 0x51CE` (the word `0x0014738D`, not in a delay slot) from user mode is handled by the emulator before CE sees it, and execution continues after it. `a0` is the operation, `a1` a buffer in the calling process and `a2` its length or capacity; the result is in `v0`.

| `a0` | Operation | Result |
| --- | --- | --- |
| 0 | Probe | `v0` = interface version (1), `v1` = largest message (65536) |
| 1 | Receive into `a1`, up to `a2` bytes | the message's length, 0 if none is waiting, or minus its length if it's bigger than `a2` (it stays queued) |
| 2 | Send `a2` bytes from `a1` | `a2`, or -1 if no host tool is connected or the message is too big |

### On the host

`headless --agent=SOCKET` and `velo --agent=SOCKET` listen on a Unix socket for one client at a time.

- Each message is a little-endian 32-bit length followed by the bytes, in both directions.
- Messages wait in a queue of 64 each way and are dropped when the client disconnects.
- Probe always answers, with or without `--agent`, and it keeps working in a state saved with the agent running.
- Buffers are read and written through CE's page tables. One that isn't paged in yet is faulted in by CE first, so it can fail like any memory access would, for example while the card an agent runs from is still being mounted after a load.

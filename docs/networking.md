# Networking and PC Link

The Velo's serial port, COM1, can be plugged into a built-in PPP network, a pseudo-terminal, a TCP port or a real serial port on the host.

| Connection | Use it for | Turn on with |
| --- | --- | --- |
| [PPP network](#ppp-network) | PC Link, RAPI, `velo-rapi`, Pocket IE through the web proxy | `--serial=net`, Devices > Network (PPP) |
| [Pseudo-terminal](#pseudo-terminal) | A terminal or PPP tools | `--serial=pty`, Devices > Pseudo-terminal |
| [TCP port](#tcp-port) | Another emulator, such as a Windows 95 PC in QEMU running H/PC Explorer | `--serial=tcp[:PORT]`, Devices > TCP Port |
| [Host serial port](#host-serial-port) | A real serial port | `--serial=/dev/cu.usbserial-XXXX`, Devices > Host Serial Port |

## Requirements

The PPP network needs a build with libslirp (included in the releases). Without it the build still works, with no Network (PPP) option. The web proxy also uses libcurl. See [building.md](building.md).

## PPP network

Devices > Network (PPP), or `--serial=net`, plugs COM1 into a built-in PPP server on a libslirp user-mode network.

1. Connecting the cable starts CE's own desktop connection: CE sends `CLIENT` and the emulator responds with `CLIENTSERVER`.
2. PPP comes up with the Velo at 10.0.2.15, the host at 10.0.2.2 and DNS at 10.0.2.3.

CE's sockets reach the host and the internet (outgoing only). 10.0.2.2 is the host's loopback.

## PC Link

With PPP up, CE connects to the desktop at 10.0.2.2 port 5679, as it did with Handheld PC Explorer, and the emulator accepts the connection, sending the ping CE 2.0 requires every few seconds.

The desktop then reaches the Velo with RAPI, CE's remote API, on its port 990. The emulator makes it available as `rapi.sock` in the data folder, with no TCP port. The protocol follows [SynCE](https://sourceforge.net/projects/synce/)'s librapi2.

CE 2.0's stock ROM lacks `rapisrv.exe`, the RAPI server; without it CE 2.0 reports "Out of Memory" when the cable is connected.

### PC Link menu

| Item | What it does |
| --- | --- |
| Send Files to Velo | Copies files into `\My Documents` |
| Copy My Documents to Mac | Copies `\My Documents`, with its folders, into a host folder |
| Shared Folder | Pairs a host folder with `\My Documents` and syncs them each time the Velo connects (see [below](#shared-folder)) |
| Sync Shared Folder Now, Stop Sharing Folder | |
| RAPI over the Network | Accepts RAPI connections from other computers (see [below](#rapi-over-the-network)) |
| Velo Settings > Set Up Pocket IE Proxy | See [Web proxy](#web-proxy) |
| Velo Settings > Connection Speed | See [Connection speed](#connection-speed) |

### Shared folder

- A file changed on one side is copied to the other.
- A file deleted on one side, and unchanged on the other since the last sync, is deleted there too (to the Trash).
- When both sides changed a file, the host keeps its copy and the Velo's arrives as `name (Velo).ext`.
- Uploads that don't fit in the Velo's free storage are skipped, and empty folders aren't removed.
- The pairing is kept as `shared_folder=` in `emu.ini`.

## velo-rapi

`velo-rapi` does the same from the command line while the emulator is running with Network (PPP) connected.

```
velo-rapi info
velo-rapi ls
velo-rapi put notes.txt
velo-rapi get Samples/Letter.pwd
velo-rapi run /Windows/pword.exe
velo-rapi sync ~/Velo
velo-rapi reg dump HKCU/Software/Apps/PocketIE
velo-rapi --help
```

- It gives up after 5 seconds if the Velo doesn't answer, and after 30 seconds of silence once connected. `--timeout=SECONDS` changes that.
- Velo paths are relative to `\My Documents` unless they start with `/` or `\`. Both separate folders.
- Registry keys start with `HKCU`, `HKLM`, `HKCR` or `HKU`.
- `--socket=PATH` picks another socket, such as one from `velo-headless --rapi=PATH`.
- `velo-rapi load` runs CE 1.0 `.load` scripts. See [installing-software.md](installing-software.md).

### RAPI over the network

PC Link > RAPI over the Network also accepts RAPI connections on port 9990 (`rapi_port` in `emu.ini`) on every network interface, so other computers, or a Mac talking to the Android app, can use it:

```
velo-rapi --connect=HOST:9990 put tool.exe /Windows/tool.exe
```

- The menu shows the address to use.
- Connections are passed to the Velo as if from the desktop, which CE 2.0's RAPI server insists on.
- `velo-headless --rapi-port=PORT` does the same.

## Connection speed

CE's desktop connection runs at 19200 baud, about 1.6 KB/s. PC Link > Connection Speed, or `velo-rapi baud 115200`, makes a faster connection the PC Connection.

| System | How |
| --- | --- |
| CE 1.0 | Adds a hidden `` `Desktop @ 115200` `` connection to the registry |
| CE 2.0 | Uses its own `` `Serial Port @ `` ones |

- The menu ticks the speed in use.
- The menu then reconnects the cable after about 10 seconds, as CE only picks up the change if the old connection has had time to settle. With `velo-rapi` it applies from the next connection.
- The speed is kept in the Velo's registry, so it lasts as long as the machine's saved state.
- At 115200 the emulated CPU sets the pace: about 1.9 KB/s at CPU Speed 1x and 5.8 KB/s at 4x.

## Web proxy

Pocket IE doesn't support modern HTTPS, so the network has a web proxy at 10.0.2.4 port 8080 that fetches pages with libcurl on the host.

### Setting it up

- PC Link > Set Up Pocket IE Proxy, or `velo-rapi proxy on`, sets it in the Velo's registry for Pocket IE's next start. CE 2.0 picks it up after a soft reset.
- By hand: in Pocket IE, View > Options > Proxy Server, tick Use Proxy Server, enter `10.0.2.4` and port `8080`.

### Using it

- Type addresses as `http://`. Pocket IE makes `https://` connections itself, not through the proxy, and they fail.
- For `http://` addresses without a port the proxy tries HTTPS first, then plain HTTP.
- Google search refuses browsers this old; [DuckDuckGo Lite](http://lite.duckduckgo.com/lite/) works.
- Only Pocket IE's requests use it, and it opens no port on the host.
- Through the proxy, `127.0.0.1` is the host's loopback.

### What it changes

Before a response reaches the Velo it:

- rewrites `https://` links and redirects to `http://`, so they come back through the proxy
- removes `<script>`, `<style>`, `<svg>` and comments, which Pocket IE would show as text
- converts UTF-8 text to Windows-1252 and drops the charset
- drops `Secure` from cookies and maps 303, 307 and 308 redirects to 301 and 302
- turns PNG, JPEG, GIF, BMP and SVG images into four-grey dithered GIFs, at the size the page's `<img width height>` gives and at most 436 pixels wide, the widest Pocket IE shows unscaled. Other formats, such as WebP, pass through unchanged.
- gzips anything else it passes through unchanged, such as JSON, if the request has `Accept-Encoding: gzip`. Pocket IE doesn't send it.

It sends a Lynx user agent upstream in place of Pocket IE's, which some sites block, and sites generally serve text browsers their simplest pages. Set it with `user_agent=` in `emu.ini` or `--user-agent=TEXT`; an empty value passes Pocket IE's own through.

## Pseudo-terminal

Devices > Pseudo-terminal, or `--serial=pty`, puts COM1 on a pty and prints its path in the title bar and on stderr, for a terminal or PPP tools.

The pty stays the same while the app runs. Choosing Pseudo-terminal again replugs the cable, so CE starts a new connection.

## TCP port

Devices > TCP Port, or `--serial=tcp`, offers COM1 as raw bytes on TCP port 9991 on every network interface, and shows the address to use. `serial_tcp_port` in `emu.ini`, or `--serial=tcp:PORT`, changes the port.

- One client at a time. The cable is plugged in while it's connected, so CE starts its desktop connection when it attaches.
- This connects COM1 to another emulator, such as a Windows 95 PC in QEMU (`-serial tcp:HOST:9991`) running H/PC Explorer. QEMU connects when the VM starts, so choose TCP Port again to replug the cable once the PC side is listening.
- `velo-headless --tcp=PORT` does the same.

## Host serial port

Devices > Host Serial Port, or `--serial=/dev/cu.usbserial-XXXX`, connects COM1 to a real port.

| Platform | Ports listed |
| --- | --- |
| macOS | `/dev/cu.*` |
| Linux | `/dev/ttyUSB*`, `/dev/ttyACM*` |

- The list is refreshed as devices come and go.
- The port is raw, with no flow control and modem lines ignored, and follows the baud rate CE sets (nearest standard rate).
- It's kept as `serial_device=` in `emu.ini`.

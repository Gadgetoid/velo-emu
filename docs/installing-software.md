# Installing CE 1.0 software

CE 1.0 programs were installed from Windows by H/PC Explorer, which ran a `.load` script for each package over RAPI. `velo-rapi load SCRIPT [DEST]`, or dropping the script on the window, does the same.

## Requirements

- The machine connected over PC Link (Network (PPP)), see [networking.md](networking.md).
- For the Power Toys example below: `python3` and [unshieldv3](https://github.com/wfr/unshieldv3).

## What a script can do

It copies files (taking the `.mips` build where there is one), creates folders, shortcuts and registry keys, writes registry strings and numbers and starts programs.

| In the script | Means |
| --- | --- |
| `.` | The script's folder as a source, and DEST as a destination |
| `%P` | DEST |
| `~ ~` | `HKEY_LOCAL_MACHINE\Software\Apps\APPNAME` |

- DEST and APPNAME come from an `Install.inf` beside the script (`InstallDir` and `AppName`).
- Otherwise DEST is `\Program Files\Accessories` and APPNAME the script's name.
- `execOnUnload` is skipped, as there's no uninstall.

## Example: Power Toys 1.0

Microsoft's Power Toys 1.0 for CE 1.0 has Cascading Menus, Mute, Pocket Paint, sound schemes, wallpapers, control panel annunciators and Remote Control. `powtoy.exe` is in archive.org's [Windows CE 1.0 Programs](https://archive.org/details/windowsce1.0) collection.

It's an InstallShield 3 package:

1. Extract the two embedded archives with unshieldv3.
2. Run each component's script.

```
python3 -c 'import struct,sys; d=open("powtoy.exe","rb").read(); p=0xcc00
while True:
    n=struct.unpack_from("<I",d,p)[0]
    if not 0<n<260: break
    size=struct.unpack_from("<I",d,p+8+n)[0]; blob=d[p+12+n:p+12+n+size]; p+=12+n+size
    if blob[:4]==b"\x13\x5d\x65\x8c": open("part%d.Z" % p,"wb").write(blob)'
mkdir powertoys
unshieldv3 extract "$(ls -S part*.Z | head -1)" powertoys
for s in annun/Annunciator cascade/Cascade mute/Mute ppaint/Ppaint rcontrol/remotecontrol sound1/Analog sound2/Metallic sound3/Organic wall/Wallpaper; do
    velo-rapi load powertoys/$s.load
done
```

Afterwards:

- Cascading Menus and Mute start straight away in the taskbar.
- Paint is in Programs > Accessories.
- The schemes are in Volume & Sounds and the wallpapers in Display.
- Remote Control needs its Windows desktop half.

#!/bin/sh
set -e
out=${1:-dist}
version=$(printf "%s" "${VERSION:-0.1+git$(date +%Y%m%d)}" | tr - "~")
arch=$(dpkg --print-architecture)
package=velo-emu
root=$(mktemp -d)
trap 'rm -rf "$root"' EXIT

make -s all headless velo-rapi velo-state icons
icons=${BUILD:-build}/icons

install -D -m 755 velo "$root/usr/bin/velo"
install -D -m 755 headless "$root/usr/bin/velo-headless"
install -D -m 755 velo-rapi "$root/usr/bin/velo-rapi"
install -D -m 755 velo-state "$root/usr/bin/velo-state"
install -D -m 644 README.md "$root/usr/share/doc/$package/README.md"
for page in using pc-card paravirtual-disk networking installing-software headless debugging optimisations android building testing hardware object-store; do install -D -m 644 "docs/$page.md" "$root/usr/share/doc/$package/docs/$page.md"; done
install -D -m 644 LICENSE "$root/usr/share/doc/$package/LICENSE"
install -D -m 644 assets/velo.svg "$root/usr/share/icons/hicolor/scalable/apps/$package.svg"
for size in 16 32 64 128 256 512; do install -D -m 644 "$icons/velo-$size.png" "$root/usr/share/icons/hicolor/${size}x${size}/apps/$package.png"; done
for licence in licences/*; do install -D -m 644 "$licence" "$root/usr/share/doc/$package/licences/$(basename "$licence")"; done

cat > "$root/usr/share/doc/$package/copyright" <<EOF
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: velo-emu
Comment: No ROMs are included.

Files: *
Copyright: 2026 Phil Howard
License: MIT
 See /usr/share/doc/$package/LICENSE
Comment: Peripheral behaviour, the memory map and the keyboard table follow CERF (MIT), see /usr/share/doc/$package/licences/MIT-CERF.txt

Files: src/vendor/stb_image.h src/vendor/stb_truetype.h
License: public-domain
 stb_image and stb_truetype by Sean Barrett, public domain.

Files: src/vendor/nanosvg.h src/vendor/nanosvgrast.h
License: Zlib
 See /usr/share/doc/$package/licences/Zlib-nanosvg.txt
EOF

mkdir -p "$root/usr/share/applications"
cat > "$root/usr/share/applications/$package.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Velo
Comment=Philips Velo 1 emulator
Exec=velo
Icon=$package
StartupWMClass=$package
Terminal=false
Categories=Emulator;Game;
EOF

work=$(mktemp -d)
mkdir -p "$work/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$package" "$package" > "$work/debian/control"
depends=$(cd "$work" && dpkg-shlibdeps -O "$root/usr/bin/velo" "$root/usr/bin/velo-headless" "$root/usr/bin/velo-rapi" "$root/usr/bin/velo-state" 2>/dev/null | sed -n 's/^shlibs:Depends=//p')
rm -rf "$work"

size=$(du -sk "$root/usr" | cut -f1)
mkdir -p "$root/DEBIAN"
cat > "$root/DEBIAN/control" <<EOF
Package: $package
Version: $version
Architecture: $arch
Maintainer: Phil Howard <phil@pimoroni.com>
Installed-Size: $size
Depends: $depends
Recommends: mtools, dosfstools, fdisk, fontconfig, fonts-dejavu-core | fonts-noto-core
Section: otherosfs
Priority: optional
Description: Philips Velo 1 handheld PC emulator
 Emulates the Philips Velo 1 (PR31500, Windows CE 1.0 and the CE 2.0 upgrade),
 with a simulated LCD, PC Card images, a PPP network with a web proxy for
 Pocket IE, and desktop connection tools.
 .
 velo is the emulator, velo-headless runs it without a window for tests and
 scripts, velo-rapi talks to a running Velo over RAPI, and velo-state reads
 files and the registry from a saved state. ROMs are not included.
EOF

mkdir -p "$out"
dpkg-deb --root-owner-group --build "$root" "$out/${package}_${version}_${arch}.deb" >/dev/null
echo "$out/${package}_${version}_${arch}.deb"

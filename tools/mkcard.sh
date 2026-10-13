#!/bin/sh
set -e
if [ $# -lt 2 ]; then
    echo "usage: mkcard.sh IMAGE SIZE_MB [DIR...]" >&2
    exit 2
fi
image=$1
size=$2
shift 2
if ! command -v mcopy > /dev/null; then
    echo "mkcard.sh: needs mtools (brew install mtools, or apt install mtools)" >&2
    exit 1
fi
if [ "$(uname -s)" != Darwin ]; then
    rm -f "$image"
    truncate -s "${size}M" "$image"
    echo 'start=1, type=06' | sfdisk --quiet --no-reread --no-tell-kernel "$image" >/dev/null
    sectors=$((size * 2048 - 1))
    mkfs.fat -F 16 -n VELOCARD -s 2 -r 512 -R 1 -a -g 16/32 -h 1 --offset 1 "$image" $((sectors / 2)) >/dev/null
else
    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT
    hdiutil create -quiet -size "${size}m" -layout MBRSPUD -fs "MS-DOS FAT16" -volname VELOCARD -type UDIF "$work/card.dmg"
    hdiutil convert -quiet "$work/card.dmg" -format UDTO -o "$work/card"
    mv "$work/card.cdr" "$image"
fi
for dir in "$@"; do mcopy -i "$image@@512" -s -Q "$dir" ::/; done
python3 "$(dirname "$0")/fatcase.py" "$image"

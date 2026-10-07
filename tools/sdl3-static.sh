#!/bin/sh
set -e
version=${SDL_VERSION:-3.4.16}
prefix=${1:-$PWD/build/sdl3}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL "https://github.com/libsdl-org/SDL/releases/download/release-$version/SDL3-$version.tar.gz" | tar -xz -C "$work"
cmake -S "$work/SDL3-$version" -B "$work/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_CAMERA=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_SENSOR=OFF
cmake --build "$work/build" --parallel
cmake --install "$work/build"
echo "$prefix/lib/pkgconfig"

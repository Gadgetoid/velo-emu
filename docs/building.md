# Building

## Requirements

| Library | Needed for |
| --- | --- |
| SDL3, zlib | Required |
| libslirp | The PPP network and PC Link (optional). 4.7, 4.8 and 4.9 all work. |
| libcurl | The web proxy for Pocket IE |
| mtools, dosfstools, fdisk | `tools/mkcard.sh` on Linux |

Without libslirp the build still works, with no Network (PPP) option.

## Make targets

| Command | Builds |
| --- | --- |
| `make` | `velo`, `velo-rapi` and `velo-state` |
| `make headless` | `headless` |
| `make app` | `Velo.app`, with its icon, its Homebrew libraries bundled and an ad-hoc signature (macOS) |
| `make apk` | `dist/velo.apk` for Android |
| `make apk-install` | Installs the APK on the phone over adb, and starts it |
| `make apk-push` | Replaces a debug install's native code without reinstalling |
| `make guest` | The Guest Additions in `guest/`, with velo-toolchain (see [paravirtual-disk.md](paravirtual-disk.md)) |
| `sh tools/mkdeb.sh dist` | The `.deb`, with dependencies from dpkg-shlibdeps |

Tests are in [testing.md](testing.md).

## macOS

```
brew install sdl3 libslirp
make
make headless
make app
```

A copy built with `make app` opens normally, without the quarantine step a downloaded one needs. `velo-rapi` and `velo-state` are inside it, in `Velo.app/Contents/MacOS`.

## Debian 13, or Ubuntu with libsdl3-dev

```
sudo apt install build-essential pkg-config libsdl3-dev libslirp-dev libcurl4-openssl-dev zlib1g-dev mtools dosfstools fdisk
make && make headless
sh tools/mkdeb.sh dist
```

## Debian 12 and Ubuntu 24.04

These lack `libsdl3-dev`, so build a static SDL3 first.

1. Install `cmake`, `curl`, `libglib2.0-dev` (for libslirp's pkg-config file) and the X11, Wayland and audio development packages, as listed in `.github/workflows/build.yml`.
2. Build:

   ```
   sh tools/sdl3-static.sh
   PKG_CONFIG_PATH=build/sdl3/lib/pkgconfig make SDL_STATIC=1 all headless
   ```

## Android

Build from macOS.

1. Install the tools:

   ```
   brew install --cask android-ndk android-commandlinetools
   brew install openjdk@21 meson ninja
   sdkmanager "platforms;android-35" "build-tools;35.0.0"
   ```

2. `make apk` builds `dist/velo.apk`. The first time, `tools/android-deps.sh` cross-builds GLib, libslirp, mbedTLS and curl into `build/android`.

Notes:

- Without a release key (`APK_KEYSTORE`, `APK_KEYSTORE_PASSWORD`, `APK_KEY_ALIAS`), the APK is a debug build signed with `~/.android/debug.keystore`.
- `make apk-install` installs with the Play Store as the installer, which gets past phones that block USB installs, such as Xiaomi's.
- `make apk-push` works because a debug build loads a newer `libmain.so` from its private files when there is one.

## Continuous integration

GitHub Actions builds `Velo.app`, the `.deb` and the Android `.apk` for each push to `main` and each pull request, runs `make check`, and attaches them to a release for each `v*` tag.

- The `.deb` is built on Debian 12 with a static SDL3, so it also installs on Debian 13 and Ubuntu 24.04 or later.
- The APK is signed with the release key in the `ANDROID_KEYSTORE` (base64), `ANDROID_KEYSTORE_PASSWORD` and `ANDROID_KEY_ALIAS` secrets, or with a throwaway debug key without them.

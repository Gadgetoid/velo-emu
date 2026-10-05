#!/bin/sh
set -e
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
api=${ANDROID_API:-28}
work=${BUILD:-build}/android
glib_version=2.90.0
slirp_version=4.9.5
mbedtls_version=3.6.7
curl_version=8.22.0

prefix=$PWD/$work/deps
sources=$work/sources
toolchain=$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin | head -1)
cmake_android="-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$api -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$prefix -DCMAKE_PREFIX_PATH=$prefix -DCMAKE_FIND_ROOT_PATH=$prefix -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON"

mkdir -p "$sources" "$prefix"
fetch() {
    if [ ! -d "$sources/$2" ]; then
        curl -sfL -o "$sources/download" "$1"
        tar xf "$sources/download" -C "$sources"
        rm "$sources/download"
    fi
}

cross=$work/meson-cross.txt
cat > "$cross" <<EOF
[binaries]
c = '$toolchain/aarch64-linux-android$api-clang'
cpp = '$toolchain/aarch64-linux-android$api-clang++'
ar = '$toolchain/llvm-ar'
strip = '$toolchain/llvm-strip'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-fPIC']
cpp_args = ['-fPIC']
pkg_config_path = '$prefix/lib/pkgconfig'

[properties]
pkg_config_libdir = '$prefix/lib/pkgconfig'

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

meson_build() {
    source=$1
    shift
    meson setup --cross-file "$cross" --prefix "$prefix" --libdir lib --default-library static --buildtype release \
        --wrap-mode default "$@" "$source/_android" "$source"
    ninja -C "$source/_android" install
}

if [ ! -f "$prefix/lib/pkgconfig/glib-2.0.pc" ]; then
    fetch "https://download.gnome.org/sources/glib/${glib_version%.*}/glib-$glib_version.tar.xz" "glib-$glib_version"
    meson_build "$sources/glib-$glib_version" --force-fallback-for=libffi,pcre2 -Dtests=false -Dintrospection=disabled \
        -Dnls=disabled -Dlibmount=disabled -Dselinux=disabled -Dxattr=false -Dman-pages=disabled -Ddocumentation=false \
        -Dsysprof=disabled -Dglib_debug=disabled
fi

if [ ! -f "$prefix/lib/pkgconfig/slirp.pc" ]; then
    fetch "https://gitlab.freedesktop.org/slirp/libslirp/-/archive/v$slirp_version/libslirp-v$slirp_version.tar.gz" "libslirp-v$slirp_version"
    meson_build "$sources/libslirp-v$slirp_version"
fi

if [ ! -f "$prefix/lib/libmbedtls.a" ]; then
    fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$mbedtls_version/mbedtls-$mbedtls_version.tar.bz2" "mbedtls-$mbedtls_version"
    cmake -S "$sources/mbedtls-$mbedtls_version" -B "$sources/mbedtls-$mbedtls_version/_android" -G Ninja $cmake_android \
        -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF
    cmake --build "$sources/mbedtls-$mbedtls_version/_android" --target install
fi

if [ ! -f "$prefix/lib/pkgconfig/libcurl.pc" ]; then
    fetch "https://curl.se/download/curl-$curl_version.tar.xz" "curl-$curl_version"
    cmake -S "$sources/curl-$curl_version" -B "$sources/curl-$curl_version/_android" -G Ninja $cmake_android \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF -DCURL_ZLIB=ON -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF -DUSE_NGHTTP2=OFF \
        -DUSE_LIBIDN2=OFF -DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF \
        -DBUILD_LIBCURL_DOCS=OFF -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF \
        -DCURL_CA_PATH=/system/etc/security/cacerts -DCURL_CA_BUNDLE=none
    cmake --build "$sources/curl-$curl_version/_android" --target install
fi

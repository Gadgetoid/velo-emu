#!/bin/sh
set -e
out=${1:-dist/velo.apk}
ndk=${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}
sdk=${ANDROID_HOME:-/opt/homebrew/share/android-commandlinetools}
min_api=24
target_api=35
sdl_version=3.4.16
version=${VERSION:-$(git describe --always --dirty 2>/dev/null || echo unknown)}
work=${BUILD:-build}/android
icons=${ICONS:-build/icons}

build_tools=$(ls -d "$sdk"/build-tools/* | sort -V | tail -1)
android_jar=$sdk/platforms/android-$target_api/android.jar
toolchain=$(ls -d "$ndk"/toolchains/llvm/prebuilt/*/bin | head -1)
cc=$toolchain/aarch64-linux-android$min_api-clang
sdl=$work/SDL3-$sdl_version

mkdir -p "$work" "$(dirname "$out")"
if [ ! -d "$sdl" ]; then
    curl -sL "https://github.com/libsdl-org/SDL/releases/download/release-$sdl_version/SDL3-$sdl_version.tar.gz" | tar xz -C "$work"
fi
if [ ! -f "$work/sdl/lib/libSDL3.so" ]; then
    cmake -S "$sdl" -B "$work/sdl-build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$min_api -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DCMAKE_INSTALL_PREFIX="$PWD/$work/sdl"
    cmake --build "$work/sdl-build"
    cmake --install "$work/sdl-build"
fi

PKG_CONFIG_LIBDIR="$PWD/$work/sdl/lib/pkgconfig" CFLAGS="-fPIC -D_GNU_SOURCE" \
    make -s BUILD="$work/obj" MENU=android CC="$cc" THREAD_LIBS= "$work/obj/libmain.so"

stage=$work/stage
rm -rf "$stage"
mkdir -p "$stage/classes" "$stage/dex" "$stage/res/mipmap-xxxhdpi" "$stage/apk/lib/arm64-v8a"
cp "$icons/velo-256.png" "$stage/res/mipmap-xxxhdpi/velo.png"

cat > "$stage/AndroidManifest.xml" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<manifest xmlns:android="http://schemas.android.com/apk/res/android" package="org.velo_emu.velo">
    <uses-feature android:glEsVersion="0x00020000" />
    <uses-feature android:name="android.hardware.touchscreen" android:required="false" />
    <uses-feature android:name="android.hardware.type.pc" android:required="false" />
    <application android:label="Velo" android:icon="@mipmap/velo" android:hasCode="true"
        android:extractNativeLibs="true" android:theme="@android:style/Theme.NoTitleBar.Fullscreen">
        <activity android:name="org.libsdl.app.SDLActivity" android:label="Velo" android:exported="true"
            android:alwaysRetainTaskState="true" android:launchMode="singleInstance" android:preferMinimalPostProcessing="true"
            android:configChanges="layoutDirection|locale|grammaticalGender|fontScale|fontWeightAdjustment|orientation|uiMode|screenLayout|screenSize|smallestScreenSize|keyboard|keyboardHidden|navigation">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity>
    </application>
</manifest>
EOF

javac -nowarn -source 11 -target 11 -Xlint:-options -cp "$android_jar" -d "$stage/classes" \
    $(find "$sdl/android-project/app/src/main/java" -name '*.java')
"$build_tools/d8" --min-api $min_api --lib "$android_jar" --output "$stage/dex" $(find "$stage/classes" -name '*.class')

"$build_tools/aapt2" compile --dir "$stage/res" -o "$stage/res.zip"
"$build_tools/aapt2" link -o "$stage/unsigned.apk" -I "$android_jar" --manifest "$stage/AndroidManifest.xml" \
    --min-sdk-version $min_api --target-sdk-version $target_api --version-code 1 --version-name "$version" \
    --debug-mode "$stage/res.zip"

"$toolchain/llvm-strip" -o "$stage/apk/lib/arm64-v8a/libSDL3.so" "$work/sdl/lib/libSDL3.so"
"$toolchain/llvm-strip" -o "$stage/apk/lib/arm64-v8a/libmain.so" "$work/obj/libmain.so"
cp "$stage/dex/classes.dex" "$stage/apk/"
(cd "$stage/apk" && zip -qr ../unsigned.apk classes.dex lib)

keystore=$HOME/.android/debug.keystore
if [ ! -f "$keystore" ]; then
    mkdir -p "$(dirname "$keystore")"
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android -alias androiddebugkey \
        -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -keysize 2048 -validity 10000
fi
"$build_tools/zipalign" -f -p 4 "$stage/unsigned.apk" "$stage/aligned.apk"
"$build_tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --out "$out" "$stage/aligned.apk"
echo "$out"

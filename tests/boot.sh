#!/bin/sh
set -e
ROM=${1:-rom/nk.bin}
OUT=${TMPDIR:-/tmp}/velo-test
mkdir -p "$OUT"
card_delete() {
    if [ "$(uname -s)" = Darwin ]; then
        mount=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$1" | awk '/VELOCARD/ {sub(/^.*\t/, ""); print}')
        rm "$mount"/$2
        rm -rf "$mount/.fseventsd" "$mount/.Spotlight-V100" "$mount/.Trashes"
        hdiutil detach -quiet "$mount"
    else
        mdel -i "$1@@512" "::$2"
    fi
}
card_extract() {
    if [ "$(uname -s)" = Darwin ]; then
        mount=$(hdiutil attach -readonly -imagekey diskimage-class=CRawDiskImage "$1" | awk '/VELOCARD/ {sub(/^.*\t/, ""); print}')
        cp -R "$mount/$2" "$3"
        hdiutil detach -quiet "$mount"
    else
        mcopy -i "$1@@512" -s -n "::$2" "$3"
    fi
}
check() {
    name=$1; expected=$2; shift 2
    ./headless "$ROM" --pgm="$OUT/$name.pgm" "$@" 2>/dev/null
    actual=$(shasum -a 256 "$OUT/$name.pgm" | cut -d' ' -f1)
    if [ -z "$expected" ]; then echo "$name $actual"; return; fi
    if [ "$actual" = "$expected" ]; then echo "ok   $name"; else echo "FAIL $name $actual"; exit 1; fi
}
CALIBRATE="--key=8:4B --key=10:4B --tap=12:240:120 --tap=14:96:48 --tap=16:96:192 --tap=18:384:192 --tap=20:384:48 --key=23:4B"
WIZARD="--tap=26:452:227 --tap=29:452:227 --tap=32:452:227 --tap=35:452:227 --tap=38:452:227 --tap=41:452:227"
check wizard   3a4cc79486ecf4d3329ab251349f2cfecaca43246e2f5c197eb7fc99e60d5d28   --seconds=8
check desktop  2eb469a6f1f8a86900314a036e30b0e725df989e4adf0962a797091ebff39033  --seconds=44 $CALIBRATE $WIZARD
./headless "$ROM" --seconds=44 --save="$OUT/desktop.state" $CALIBRATE $WIZARD 2>/dev/null
check resumed 2eb469a6f1f8a86900314a036e30b0e725df989e4adf0962a797091ebff39033 --seconds=1 --load="$OUT/desktop.state"
if ./headless "$ROM" --host-time --seconds=1 2>&1 | grep -q "clock: set from the host"; then echo "ok   host_time"; else echo "FAIL host_time"; exit 1; fi
if ./headless "$ROM" --debug-output --seconds=1 2>&1 | grep -q "^debug: Pegasus Kernel for MIPS"; then echo "ok   debug_output"; else echo "FAIL debug_output"; exit 1; fi
check soft_reset ded10bd9062935ba803cf78c2adf05aa3865f39b8486e1f36a450a5dc6f80f99 --seconds=30 --load="$OUT/desktop.state" --soft-reset=1
./headless "$ROM" --pgm="$OUT/start_continuous.pgm" --seconds=47 $CALIBRATE $WIZARD --tap=45:15:227 2>/dev/null
./headless "$ROM" --pgm="$OUT/start_resumed.pgm" --seconds=3 --load="$OUT/desktop.state" --tap=1:15:227 2>/dev/null
if cmp -s "$OUT/start_continuous.pgm" "$OUT/start_resumed.pgm"; then echo "ok   resume matches continuous run"; else echo "FAIL resume differs from continuous run"; exit 1; fi
check suspend_resume 2eb469a6f1f8a86900314a036e30b0e725df989e4adf0962a797091ebff39033 --seconds=10 --load="$OUT/desktop.state" --power=2 --power=6
sh tools/mkcard.sh "$OUT/card.img" 8 tests/card/HELLO
CARD="--card=$OUT/card.img --tap=4:30:25:0.08 --tap=4.12:30:25:0.08 --tap=7:262:65:0.08 --tap=7.12:262:65:0.08"
check card_listing b92dd3e0c4dbf3472d5ebc40937767f061a626fca3209eed6682393c77530786 --seconds=10 --load="$OUT/desktop.state" $CARD
PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
./headless "$ROM" --seconds=20 --load="$OUT/desktop.state" --gdb="$PORT" > "$OUT/gdb.log" 2>&1 &
if python3 tests/gdb_client.py "$PORT" > "$OUT/gdb_client.log" 2>&1; then echo "ok   gdb_stub"; else echo "FAIL gdb_stub"; cat "$OUT/gdb_client.log" "$OUT/gdb.log"; exit 1; fi
wait
if pkg-config --exists slirp; then
    ./headless "$ROM" --seconds=10 --load="$OUT/desktop.state" --net=1 > "$OUT/ppp.log" 2>&1
    if grep -q "IPCP up" "$OUT/ppp.log"; then echo "ok   ppp_online"; else echo "FAIL ppp_online"; exit 1; fi
    if grep -q "desktop: connection" "$OUT/ppp.log"; then echo "ok   desktop_accepted"; else echo "FAIL desktop_accepted"; exit 1; fi
    RECONNECT="--tap=6:396:229:0.08 --tap=6.12:396:229:0.08 --tap=8:330:154 --tap=10:15:227:0.1 --tap=11:70:93:0.1 --tap=14:115:72:0.08 --tap=14.12:115:72:0.08 --tap=17:190:72:0.08 --tap=17.12:190:72:0.08"
    ./headless "$ROM" --seconds=24 --load="$OUT/desktop.state" --net=1 $RECONNECT > "$OUT/reconnect.log" 2>&1
    if [ "$(grep -c "IPCP up" "$OUT/reconnect.log")" = 2 ]; then echo "ok   ppp_reconnect"; else echo "FAIL ppp_reconnect"; exit 1; fi
    POWER_CYCLE="--power=6 --power=10 --tap=13:15:227:0.1 --tap=14:70:93:0.1 --tap=17:115:72:0.08 --tap=17.12:115:72:0.08 --tap=20:190:72:0.08 --tap=20.12:190:72:0.08"
    ./headless "$ROM" --seconds=24 --load="$OUT/desktop.state" --net=1 $POWER_CYCLE > "$OUT/power_cycle.log" 2>&1
    if [ "$(grep -c "IPCP up" "$OUT/power_cycle.log")" = 2 ]; then echo "ok   ppp_power_cycle"; else echo "FAIL ppp_power_cycle"; exit 1; fi
fi
if pkg-config --exists slirp libcurl; then
    PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    python3 -m http.server "$PORT" --bind 127.0.0.1 --directory tests/web >/dev/null 2>&1 &
    SERVER=$!
    trap 'kill $SERVER 2>/dev/null || true' EXIT
    sleep 1
    PAGE=$(./proxycheck "http://127.0.0.1:$PORT/page.html" 2>/dev/null | iconv -f WINDOWS-1252 -t UTF-8)
    if echo "$PAGE" | grep -q 'Café “quoted” — dash</p><a href="http://example.com/next">next</a></body>' && ! echo "$PAGE" | grep -qiE 'script|style|svg|hidden'; then echo "ok   proxy_rewrite"; else echo "FAIL proxy_rewrite"; exit 1; fi
    BODY='import sys, struct, hashlib; head, body = sys.stdin.buffer.read().split(b"\r\n\r\n", 1); print(body[:6].decode("latin-1"), *struct.unpack("<HH", body[6:10]), hashlib.sha256(body).hexdigest())'
    SVG=$(./proxycheck "http://127.0.0.1:$PORT/images.html" "http://127.0.0.1:$PORT/shapes.svg" 2>/dev/null | python3 -c "$BODY")
    PNG=$(./proxycheck "http://127.0.0.1:$PORT/gradient.png" 2>/dev/null | python3 -c "$BODY")
    if [ "$SVG" = "GIF89a 100 50 4952502eef863d6cd28acd031a890c4d93b43269bebd712c05318955547ac371" ] && [ "${PNG% *}" = "GIF89a 436 218" ]; then echo "ok   proxy_images"; else echo "FAIL proxy_images $SVG / $PNG"; exit 1; fi
    IE="--tap=2:112:20:0.08 --tap=2.12:112:20:0.08 --tap=6:97:14:0.1 --tap=7:110:151:0.1 --tap=8.5:298:43:0.1 --tap=9.5:120:107:0.1 --tap=10.2:210:171:0.1 --type=10.6:10.0.2.4 --tap=11.5:365:171:0.1 --key=12:39 --key=12.2:39 --key=12.4:39 --type=12.8:8080 --key=14:4B --net=15 --tap=22:16:14:0.1 --tap=23:36:49:0.1"
    check proxy_browse 9c5af91bed6b16480a9e586f011716f6c68b3d9163eb8d0e338d10b7d08f1ca7 --seconds=40 --load="$OUT/desktop.state" $IE "--type=24:http://127.0.0.1:$PORT/page.html\\n"
fi
if pkg-config --exists slirp; then
    SOCKET="${TMPDIR:-/tmp}/velo-test-$$.sock"
    RAPI_PORT=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
    ./headless "$ROM" --seconds=100000 --realtime=10 --load="$OUT/desktop.state" --net=1 --rapi="$SOCKET" --rapi-port="$RAPI_PORT" >/dev/null 2>&1 &
    EMULATOR=$!
    trap 'kill $SERVER $EMULATOR 2>/dev/null || true' EXIT
    for attempt in 1 2 3 4 5 6 7 8 9 10; do ./velo-rapi --socket="$SOCKET" info >/dev/null 2>&1 && break; sleep 1; done
    head -c 20000 /dev/urandom > "$OUT/blob.bin"
    rm -f "$OUT/blob.back"
    if ./velo-rapi --socket="$SOCKET" put "$OUT/blob.bin" && ./velo-rapi --socket="$SOCKET" get blob.bin "$OUT/blob.back" && cmp -s "$OUT/blob.bin" "$OUT/blob.back" && ./velo-rapi --socket="$SOCKET" rm blob.bin; then echo "ok   rapi_roundtrip"; else echo "FAIL rapi_roundtrip"; exit 1; fi
    if ./velo-rapi --connect="127.0.0.1:$RAPI_PORT" ls / | grep -q "My Documents"; then echo "ok   rapi_tcp"; else echo "FAIL rapi_tcp"; exit 1; fi
    SHARED="$OUT/shared"
    rm -rf "$SHARED"
    mkdir -p "$SHARED/Notes"
    echo "from the mac" > "$SHARED/Notes/mac.txt"
    export XDG_DATA_HOME="$OUT/data"
    mkdir -p "$XDG_DATA_HOME/velo-emu"
    rm -f "$XDG_DATA_HOME/velo-emu/sync-manifest.txt"
    ./velo-rapi --socket="$SOCKET" sync "$SHARED" >/dev/null
    echo "from the velo" > "$OUT/velo.txt"
    ./velo-rapi --socket="$SOCKET" put "$OUT/velo.txt" Notes/velo.txt
    rm "$SHARED/Notes/mac.txt"
    ./velo-rapi --socket="$SOCKET" sync "$SHARED" >/dev/null
    LISTING=$(./velo-rapi --socket="$SOCKET" ls Notes)
    if [ -f "$SHARED/Samples/Memo.pwd" ] && grep -q "from the velo" "$SHARED/Notes/velo.txt" && ! echo "$LISTING" | grep -q mac.txt; then echo "ok   rapi_sync"; else echo "FAIL rapi_sync"; exit 1; fi
    ./velo-rapi --socket="$SOCKET" rm Notes/velo.txt
    ./velo-rapi --socket="$SOCKET" rmdir Notes
    unset XDG_DATA_HOME
    kill $EMULATOR
    wait $EMULATOR 2>/dev/null || true
    ./headless "$ROM" --seconds=100000 --realtime=10 --load="$OUT/desktop.state" --net=1 --rapi="$SOCKET" --save="$OUT/setup.state" >/dev/null 2>&1 &
    EMULATOR=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do ./velo-rapi --socket="$SOCKET" info >/dev/null 2>&1 && break; sleep 1; done
    ./velo-rapi --socket="$SOCKET" baud 115200 && ./velo-rapi --socket="$SOCKET" proxy on
    PROXY=$(./velo-rapi --socket="$SOCKET" reg get HKCU/Software/Apps/PocketIE ProxyServer)
    { for i in $(seq 600); do echo "line $i of a file read back from a saved state"; done; head -c 6000 /dev/urandom; } > "$OUT/mixed.bin"
    ./velo-rapi --socket="$SOCKET" put "$OUT/mixed.bin"
    kill -TERM $EMULATOR
    wait $EMULATOR
    if [ "$PROXY" = 'string "10.0.2.4"' ] && ./headless "$ROM" --load="$OUT/setup.state" --cable=2 --seconds=6 2>&1 | grep -q "at 115200 baud"; then echo "ok   rapi_setup"; else echo "FAIL rapi_setup"; exit 1; fi
    rm -f "$OUT/mixed.back"
    if [ "$(./velo-state "$OUT/setup.state" reg get HKCU/Software/Apps/PocketIE ProxyServer)" = 'string "10.0.2.4"' ] &&
       ./velo-state "$OUT/setup.state" get mixed.bin "$OUT/mixed.back" && cmp -s "$OUT/mixed.bin" "$OUT/mixed.back" &&
       ./velo-state "$OUT/desktop.state" diff "$OUT/setup.state" | grep -q '^+ HKCU\\Software\\Apps\\PocketIE\\UseProxy = dword 1'; then echo "ok   state_reader"; else echo "FAIL state_reader"; exit 1; fi
    ./headless "$ROM" --seconds=100000 --realtime=10 --load="$OUT/desktop.state" --net=1 --rapi="$SOCKET" >/dev/null 2>&1 &
    EMULATOR=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do ./velo-rapi --socket="$SOCKET" info >/dev/null 2>&1 && break; sleep 1; done
    LOADED=$(./velo-rapi --socket="$SOCKET" load tests/load/Test.load /Windows/LoadTest >/dev/null && {
        ./velo-rapi --socket="$SOCKET" get /Windows/LoadTest/tool.exe "$OUT/tool.exe" && cat "$OUT/tool.exe"
        ./velo-rapi --socket="$SOCKET" get "/Windows/Load Test/Read Me.txt" "$OUT/readme.txt" && cat "$OUT/readme.txt"
        for link in Tool Volume Root; do ./velo-rapi --socket="$SOCKET" get "/Windows/Load Test/$link.lnk" "$OUT/link.lnk" && cat "$OUT/link.lnk" && echo; done
        ./velo-rapi --socket="$SOCKET" ls "/Windows/Load Test" | grep -c "Empty"
        ./velo-rapi --socket="$SOCKET" reg get HKLM/Software/LoadTest Name
        ./velo-rapi --socket="$SOCKET" reg get HKLM/Software/LoadTest List
        ./velo-rapi --socket="$SOCKET" reg ls HKCR/.loadtest
        ./velo-rapi --socket="$SOCKET" reg ls "HKLM/Software/Apps/Load Test"
        ./velo-rapi --socket="$SOCKET" ls /Windows/LoadTest | grep -c never || true
    })
    kill $EMULATOR
    wait $EMULATOR 2>/dev/null || true
    EXPECTED='mips build
plain file
26#\Windows\LoadTest\tool.exe
59#"\Windows\ctlpnl.exe" \Windows\sounds.cpl,Volume & Sounds,0
1#\
1
string "Load Test"
multi "one|two|three"
Default = string "loadtestfile"
Count = dword 42 (0x2a)
Tool = string "\Windows\LoadTest\tool.exe"
0'
    if [ "$LOADED" = "$EXPECTED" ]; then echo "ok   rapi_load"; else echo "FAIL rapi_load"; echo "$LOADED"; exit 1; fi
    rm -rf "$OUT/FRAG"
    mkdir -p "$OUT/FRAG"
    for i in 1 2 3 4 5 6; do head -c 8000 /dev/urandom > "$OUT/FRAG/gap$i.bin"; head -c 1000 /dev/urandom > "$OUT/FRAG/keep$i.bin"; done
    head -c 40000 /dev/urandom > "$OUT/large.bin"
    sh tools/mkcard.sh "$OUT/frag.img" 8 "$OUT/FRAG"
    card_delete "$OUT/frag.img" "FRAG/gap*.bin"
    ./headless "$ROM" --seconds=100000 --realtime=10 --load="$OUT/desktop.state" --card="$OUT/frag.img" --net=1 --rapi="$SOCKET" >/dev/null 2>&1 &
    EMULATOR=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do ./velo-rapi --socket="$SOCKET" info >/dev/null 2>&1 && break; sleep 1; done
    ./velo-rapi --socket="$SOCKET" put "$OUT/large.bin" "/PC Card/FRAG/large.bin"
    kill $EMULATOR
    wait $EMULATOR 2>/dev/null || true
    rm -rf "$OUT/FRAG-BACK"
    card_extract "$OUT/frag.img" FRAG "$OUT/FRAG-BACK"
    intact=yes
    cmp -s "$OUT/FRAG-BACK/large.bin" "$OUT/large.bin" || intact=no
    for i in 1 2 3 4 5 6; do cmp -s "$OUT/FRAG-BACK/keep$i.bin" "$OUT/FRAG/keep$i.bin" || intact=no; done
    if [ $intact = yes ]; then echo "ok   card_fragmented_write"; else echo "FAIL card_fragmented_write"; exit 1; fi
    rm -rf "$OUT/ALPHA" "$OUT/BRAVO"
    mkdir -p "$OUT/ALPHA" "$OUT/BRAVO"
    echo alpha > "$OUT/ALPHA/alpha.txt"
    echo bravo > "$OUT/BRAVO/bravo.txt"
    sh tools/mkcard.sh "$OUT/alpha.img" 8 "$OUT/ALPHA"
    sh tools/mkcard.sh "$OUT/bravo.img" 8 "$OUT/BRAVO"
    ./headless "$ROM" --seconds=8 --load="$OUT/desktop.state" --card="$OUT/alpha.img" --save="$OUT/alpha.state" >/dev/null 2>&1
    ./headless "$ROM" --seconds=100000 --realtime=10 --load="$OUT/alpha.state" --card="$OUT/bravo.img" --net=1 --rapi="$SOCKET" >/dev/null 2>&1 &
    EMULATOR=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do ./velo-rapi --socket="$SOCKET" info >/dev/null 2>&1 && break; sleep 1; done
    SWAPPED=$(./velo-rapi --socket="$SOCKET" ls "/PC Card" 2>&1)
    kill $EMULATOR
    wait $EMULATOR 2>/dev/null || true
    if echo "$SWAPPED" | grep -q "BRAVO" && ! echo "$SWAPPED" | grep -q "ALPHA"; then echo "ok   card_swap"; else echo "FAIL card_swap $SWAPPED"; exit 1; fi
    cp "$OUT/alpha.img" "$OUT/gone.img"
    ./headless "$ROM" --seconds=8 --load="$OUT/desktop.state" --card="$OUT/gone.img" --save="$OUT/gone.state" >/dev/null 2>&1
    rm "$OUT/gone.img"
    ./headless "$ROM" --seconds=100000 --realtime=10 --load="$OUT/gone.state" --card="$OUT/bravo.img" --net=1 --rapi="$SOCKET" >/dev/null 2>&1 &
    EMULATOR=$!
    for attempt in 1 2 3 4 5 6 7 8 9 10; do ./velo-rapi --socket="$SOCKET" info >/dev/null 2>&1 && break; sleep 1; done
    REPLACED=$(./velo-rapi --socket="$SOCKET" ls "/PC Card" 2>&1)
    kill $EMULATOR
    wait $EMULATOR 2>/dev/null || true
    if echo "$REPLACED" | grep -q "BRAVO"; then echo "ok   card_missing_from_state"; else echo "FAIL card_missing_from_state $REPLACED"; exit 1; fi
fi
if ./headless "$ROM" --seconds=4 --load="$OUT/desktop.state" --backlight=2 --trace-pc 2>&1 | grep "^t=" | tail -1 | grep -q "backlight=1"; then echo "ok   backlight_button"; else echo "FAIL backlight_button"; exit 1; fi
SYSINFO="--tap=46:15:227:0.1 --tap=47:60:133:0.1 --tap=48.5:130:133:0.1 --tap=52:262:150:0.08 --tap=52.12:262:150:0.08"
check memory_16mb 151d7d109bd5e68c370edf0069c58225c69d0f9bbd9c97016820b08c64ebdea5 --memory=16 --seconds=56 $CALIBRATE $WIZARD $SYSINFO
check memory_32mb c6b7c1de7831be7af001cf41e8d678a21f222b356beeae4440878c9602a3d535 --memory=32 --seconds=56 $CALIBRATE $WIZARD $SYSINFO
check screen_640x480_wizard 93ae4f60b5550cd914c4efadc88a6e22f31f0fde9fbc8a7cb8ddd4d8a162bbd7 --screen=640x480 --seconds=8
SCREEN_SETUP="--key=8:4B --key=10:4B --tap=12:320:240 --tap=14:128:96 --tap=16:128:384 --tap=18:512:384 --tap=20:512:96 --key=23:4B --tap=27:631:16:0.1 --tap=31:631:16:0.1 --tap=35:631:16:0.1 --tap=39:631:16:0.1 --tap=43:631:16:0.1 --tap=47:456:231 --tap=50:456:231 --tap=53:456:231"
./headless "$ROM" --screen=640x480 --seconds=56 --save="$OUT/screen.state" $SCREEN_SETUP 2>/dev/null
check screen_640x480_resumed f897b6341b6ffdf3a6071f4de8b86c217e1fa764343c39e3e26caf04d2e6d203 --seconds=1 --load="$OUT/screen.state"
check screen_640x480_typing 18f2dbe96c51590ce145639f6a1873e6a5f4f60367d1273313e545ef1f40c5ff --seconds=14 --load="$OUT/screen.state" --tap=1:360:45:0.08 --tap=1.12:360:45:0.08 "--type=5:Resumed at 640 x 480."
CE2_ROM=${2:-rom/nk-ce2.bin}
if [ -f "$CE2_ROM" ]; then
    CE1_ROM=$ROM
    ROM=$CE2_ROM
    if ./headless "$ROM" --host-time --seconds=1 2>&1 | grep -q "clock: set from the host"; then echo "ok   ce2_host_time"; else echo "FAIL ce2_host_time"; exit 1; fi
    if ./headless "$ROM" --debug-output --seconds=3 2>&1 | grep -q "^debug: Configuring 480x240, 4bpp display"; then echo "ok   ce2_debug_output"; else echo "FAIL ce2_debug_output"; exit 1; fi
    check ce2_desktop aa64f3fba1031ff617de1871716776d2323a5a189c896269be719d5317119e5e --seconds=20
    ./headless "$ROM" --seconds=20 --save="$OUT/ce2_desktop.state" 2>/dev/null
    if [ "$(./velo-state "$OUT/ce2_desktop.state" reg get HKLM/init Launch50)" = 'string "explorer.exe"' ] &&
       ./velo-state "$OUT/ce2_desktop.state" ls | grep -q "36  Shortcut to Templates.lnk"; then echo "ok   ce2_state_reader"; else echo "FAIL ce2_state_reader"; exit 1; fi
    check ce2_memory_20mb 66a2cc77cfed28615b0486c02a749163fc13755aa5cc01348fbc25e689cbaa05 --memory=20 --seconds=25
    check ce2_start_uncalibrated aa5f6f4b5de495a83c8c53ce380a80692321db5aec941043d2ec02839351d20b --tap=19:15:227:0.1 --seconds=22
    check ce2_double_tap_slow 56b264bbdadf6f1db2f529f1327ca958870aad786d21518c320638ab8b018b07 --tap=21:30:20:0.08 --tap=21.35:30:20:0.08 --seconds=24
    check ce2_double_tap 56b264bbdadf6f1db2f529f1327ca958870aad786d21518c320638ab8b018b07 --tap=21:30:20:0.08 --tap=21.12:30:20:0.08 --seconds=24
    ./headless "$ROM" --tap=21:15:227:0.1 --key=22:30 "--type=23.5:reset\\n" --tap=27:200:180:0.1 --seconds=60 --pgm="$OUT/ce2_warm_reset.pgm" > "$OUT/ce2_warm_reset.log" 2>&1
    if grep -q "boot block: warm reset" "$OUT/ce2_warm_reset.log" && [ "$(shasum -a 256 "$OUT/ce2_warm_reset.pgm" | cut -d' ' -f1)" = aa64f3fba1031ff617de1871716776d2323a5a189c896269be719d5317119e5e ]; then echo "ok   ce2_warm_reset"; else echo "FAIL ce2_warm_reset"; exit 1; fi
    check ce2_type_symbols 9ff064a21521aebd136842b817804a7d82a05e7d1c619698fc1dfb785f29155e --tap=21:15:227:0.1 --key=22:30 '--type=23.5:\=[]' --seconds=26
    python3 -c 'import struct, sys; d = open(sys.argv[1], "rb").read(); open(sys.argv[2], "wb").write(b"B000FF\n" + struct.pack("<II", 0x90001000, len(d)) + struct.pack("<III", 0x90001000, len(d), sum(d)) + d + struct.pack("<III", 0, 0x90001000, 0))' "$CE2_ROM" "$OUT/ce2.b000ff"
    ROM="$OUT/ce2.b000ff"
    check ce2_b000ff aa64f3fba1031ff617de1871716776d2323a5a189c896269be719d5317119e5e --seconds=20
    ROM=$CE2_ROM
    check ce2_screen_640x240 06c0e1c97a9d0fa895e9a3d2c4cd85ee079363195cac96e35a699bb48944cb6e --screen=640x240 --seconds=20
    if ./headless "$ROM" --screen=640x480 --seconds=1 2>&1 | grep -q "can't run at 640x480"; then echo "ok   ce2_screen_unsupported"; else echo "FAIL ce2_screen_unsupported"; exit 1; fi
    ROM=$CE1_ROM
fi

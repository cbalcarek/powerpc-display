#!/bin/sh
# Builds the Mac OS 9 receiver with Retro68 (Linux or macOS) and writes
#   releases/PowerPC-Display-OS9.bin   MacBinary: copy to the old Mac, double-click
#   releases/PowerPC-Display-OS9.dsk   HFS disk image with the app (emulators)
# Needs Retro68's PowerPC toolchain; RETRO68 points at its install folder.
set -e
cd "$(dirname "$0")"
RETRO68="${RETRO68:-$HOME/Retro68-build/toolchain}"
B=build
cmake -S . -B $B -DCMAKE_TOOLCHAIN_FILE="$RETRO68/powerpc-apple-macos/cmake/retroppc.toolchain.cmake" >/dev/null
cmake --build $B
mkdir -p ../releases
python3 - "$B/PowerPCDisplay.bin" ../releases/PowerPC-Display-OS9.bin <<'PY'
import sys, struct
d = bytearray(open(sys.argv[1], 'rb').read())
name = b'PowerPC Display'
d[1] = len(name); d[2:65] = name.ljust(63, b'\0')
def crc(b):                      # MacBinary II: CRC-16/XMODEM of bytes 0-123
    c = 0
    for x in b:
        c ^= x << 8
        for _ in range(8): c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c
d[124:126] = struct.pack('>H', crc(d[:124]))
open(sys.argv[2], 'wb').write(d)
PY
# the disk image: same app, named properly, on a volume called PowerPC Display
cp $B/PowerPCDisplay.dsk ../releases/PowerPC-Display-OS9.dsk
export HOME="${TMPDIR:-/tmp}"     # hfsutils keeps its state in ~/.hcwd
"$RETRO68/bin/hmount" ../releases/PowerPC-Display-OS9.dsk >/dev/null
"$RETRO68/bin/hrename" ":PowerPCDisplay" ":PowerPC Display"
"$RETRO68/bin/hrename" ":" "PowerPC Display:" 2>/dev/null || true
"$RETRO68/bin/hls" -l
"$RETRO68/bin/humount"
echo "Wrote releases/PowerPC-Display-OS9.bin and releases/PowerPC-Display-OS9.dsk"

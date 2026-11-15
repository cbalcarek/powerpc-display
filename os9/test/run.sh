#!/bin/sh
# Runs the Mac OS 9 receiver's core (os9/rx_core.c) on Linux against the fake
# Macs in test/sim: keyboard/mouse signing and sound, the picture stream, and
# pairing. Usage: sh os9/test/run.sh   (needs gcc; ~40 s)
set -e
cd "$(dirname "$0")"
R=../..
C=$R/host/Sources/CCodec
SIM=$R/test/sim
OUT=${TMPDIR:-/tmp}/os9sim.$$
mkdir -p "$OUT"
cp -R $SIM/fixture/home "$OUT/home"
cp $SIM/fixture/hostsim.key "$OUT/"
CODEC="$C/g4codec.c $C/g4auth.c $C/g4nacl.c $C/g4yuv_altivec.c"
gcc -O1 -g -Wall -Wno-unused-parameter -I$C/include -I.. -o "$OUT/rx" rx_test_plat.c ../rx_core.c $CODEC -lm
gcc -O1 -w -I$C/include -o "$OUT/fake_mac_input" $SIM/fake_mac_input.c $CODEC -lm
gcc -O1 -w -I$C/include -o "$OUT/fake_mac_stream" $SIM/fake_mac_stream.c $CODEC -lm
cd "$OUT"
fail=0

echo "== keyboard, mouse and sound =="
HOME=$PWD/home timeout 9 ./rx > rx1.log 2>&1 &
sleep 0.3
if timeout 8 ./fake_mac_input > mac1.log; then echo "ok"; else echo "FAILED"; fail=1; fi
wait || true
grep "signed" mac1.log
grep "FAKE: played" rx1.log || { echo "no sound played"; fail=1; }

echo "== picture stream (32-bit) =="
NOKEYS=1 DEPTH=32 HOME=$PWD/home timeout 22 ./rx > rx2.log 2>&1 &
sleep 0.3
if STREAM=1 timeout 20 ./fake_mac_stream known > mac2.log; then echo "ok"; else echo "FAILED"; fail=1; fi
wait || true
grep "PONG" mac2.log || true

[ $fail = 0 ] && echo "ALL OS 9 CORE SIMULATIONS PASSED" || echo "Logs: $OUT"
exit $fail

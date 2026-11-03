#!/bin/sh
# Runs the PowerPC client (g4/client.c) on Linux against fake Tiger APIs and
# a fake Mac, so the PowerPC side can be checked without Tiger:
#   1. input + sound: signed keyboard/mouse messages, key order, Command-Escape
#      never sent, the sound played to the end
#   2. picture stream: handshake, frames, pings, ACKs, giving up after silence
# Usage: sh test/sim/run.sh        (needs gcc; ~30 s)
set -e
cd "$(dirname "$0")"
R=../..
C=$R/host/Sources/CCodec
OUT=${TMPDIR:-/tmp}/ppcsim.$$
mkdir -p "$OUT"
cp -R fixture/home "$OUT/home"
cp fixture/hostsim.key "$OUT/"
CODEC="$C/g4codec.c $C/g4auth.c $C/g4nacl.c $C/g4yuv_altivec.c"
gcc -O1 -g -w -pthread -Dmain=client_main -Iinclude -I$C/include -I$R/g4 -c $R/g4/client.c -o "$OUT/client.o"
gcc -O1 -g -w -pthread -Iinclude -I$C/include -I$R/g4 -o "$OUT/fake_tiger" "$OUT/client.o" fake_tiger.c $CODEC -lm
gcc -O1 -w -I$C/include -o "$OUT/fake_mac_input" fake_mac_input.c $CODEC -lm
gcc -O1 -w -I$C/include -o "$OUT/fake_mac_stream" fake_mac_stream.c $CODEC -lm
cd "$OUT"
fail=0

echo "== keyboard, mouse and sound =="
HOME=$PWD/home MODE=raw timeout 9 ./fake_tiger -v > tiger1.log 2>&1 &
sleep 0.3
if timeout 8 ./fake_mac_input > mac1.log; then echo "ok"; else echo "FAILED"; fail=1; fi
wait || true
grep "signed" mac1.log
grep -a "FAKE: played" tiger1.log || { echo "no sound played"; fail=1; }

echo "== picture stream =="
NOKEYS=1 HOME=$PWD/home MODE=gl2d timeout 22 ./fake_tiger -v > tiger2.log 2>&1 &
sleep 0.3
if STREAM=1 timeout 20 ./fake_mac_stream known > mac2.log; then echo "ok"; else echo "FAILED"; fail=1; fi
wait || true
grep "PONG" mac2.log

[ $fail = 0 ] && echo "ALL SIMULATIONS PASSED" || echo "Logs: $OUT"
exit $fail

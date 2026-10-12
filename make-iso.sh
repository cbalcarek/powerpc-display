#!/bin/sh
# Builds ~/Downloads/g4display.iso (PowerPC client source only) for the Tiger VM in UTM,
# or to burn to a CD for a real PowerPC Mac.
# Usage: sh ~/Downloads/g4display/make-iso.sh
set -e
cd "$(dirname "$0")"
OUT="$(cd .. && pwd)/g4display.iso"
TMP="$(mktemp -d)"
mkdir -p "$TMP/g4display/host/Sources"
mkdir -p "$TMP/g4display/g4"
for f in client.c app.m g4client.h AppIcon.icns update.sh "Build PowerPC Display.command"; do
    cp "g4/$f" "$TMP/g4display/g4/"
done
cp -R host/Sources/CCodec "$TMP/g4display/host/Sources/"
cp README.md "$TMP/g4display/"
chmod +x "$TMP"/g4display/g4/*.command "$TMP"/g4display/g4/*.sh
hdiutil makehybrid -ov -iso -hfs -default-volume-name g4display -o "$OUT" "$TMP" >/dev/null
rm -rf "$TMP"
echo "Built $OUT"
echo "In UTM: eject the old g4display disc in Tiger, then choose this file again from the CD icon."
echo "In Tiger: copy the g4display folder from the disc to your home folder and double-click g4/Build PowerPC Display.command."

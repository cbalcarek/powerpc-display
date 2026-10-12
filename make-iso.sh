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
# where the PowerPC build can send its release zip back to this Mac:
# UTM Emulated VLAN, UTM Shared Network, then this Mac's Bonjour name
echo "10.0.2.2 192.168.64.1 $(scutil --get LocalHostName 2>/dev/null || hostname -s).local" > "$TMP/g4display/g4/.m1hosts"
hdiutil makehybrid -ov -iso -hfs -default-volume-name g4display -o "$OUT" "$TMP" >/dev/null
rm -rf "$TMP"
echo "Built $OUT"
echo "In UTM: eject the old g4display disc in Tiger, then choose this file again from the CD icon."
echo "In Tiger: open the disc and double-click g4display/g4/Build PowerPC Display.command (it copies itself to ~/g4display)."
echo "Leave Share with PowerPC Mac.command running here and the built app comes back to releases/."

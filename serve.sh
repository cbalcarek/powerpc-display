#!/bin/sh
# Run on your Apple Silicon Mac and leave it running: shares this folder so a
# PowerPC Mac (or the Tiger VM) can fetch the latest source with g4/update.sh.
cd "$(dirname "$0")"
echo "Serving $(pwd) on port 8000 — press Control-C to stop."
NAME="$(scutil --get LocalHostName 2>/dev/null || hostname -s).local"
echo
echo "On the PowerPC Mac (G3, G4 or G5), in Terminal:"
[ -e xcode.dmg ] && echo "  Xcode installer:  curl -O http://$NAME:8000/xcode.dmg && open xcode.dmg"
echo "  PowerPC Display:  mkdir -p ~/g4display/g4 && cd ~/g4display/g4 && curl -O http://$NAME:8000/g4/update.sh && sh update.sh $NAME"
echo "  Later updates:    sh ~/g4display/g4/update.sh $NAME"
echo
exec python3 -m http.server 8000 --bind 0.0.0.0

#!/bin/sh
# Double-click on your Apple Silicon Mac: builds "PowerPC Display.app",
# installs it in /Applications and opens it.
cd "$(dirname "$0")/host" || exit 1
sh build-app.sh || { echo; echo "Build failed - copy the errors above to Claude."; exit 1; }
# The Screen Recording permission belongs to the app's signature. If that
# changed (first build with the stable certificate, or an ad-hoc build),
# the old entry in System Settings stays switched on but no longer applies,
# so clear it and macOS asks once, cleanly.
OLD_REQ="$(codesign -d -r- "/Applications/PowerPC Display.app" 2>/dev/null | grep designated)"
NEW_REQ="$(codesign -d -r- "build/PowerPC Display.app" 2>/dev/null | grep designated)"
pkill -x PowerPCDisplay >/dev/null 2>&1
pkill -x g4host >/dev/null 2>&1          # the old "G4 Display" app
sleep 1
if [ -d "/Applications/G4 Display.app" ]; then
    echo "Removing the old G4 Display app (now PowerPC Display)..."
    rm -rf "/Applications/G4 Display.app"
fi
rm -rf "/Applications/PowerPC Display.app"
mv "build/PowerPC Display.app" /Applications/ || exit 1
if [ "$OLD_REQ" != "$NEW_REQ" ]; then
    tccutil reset ScreenCapture local.ppcdisplay.host >/dev/null 2>&1
    echo
    echo "The app's signature changed, so its old Screen Recording permission was cleared."
    echo "When macOS asks, allow PowerPC Display, then quit and reopen it (once)."
    echo
fi
# where the releases folder is: the app offers PowerPC Macs the newest
# releases/PowerPC-Display-PPC.zip (the Tiger build sends it there)
defaults write local.ppcdisplay.host releasesDir "$(cd .. && pwd)/releases"
open "/Applications/PowerPC Display.app"
mkdir -p ../releases
ditto -c -k --keepParent "/Applications/PowerPC Display.app" ../releases/PowerPC-Display-M1.zip &&
    echo "Saved releases/PowerPC-Display-M1.zip (for GitHub releases)."
echo "Installed and opened /Applications/PowerPC Display.app - you can close this window."

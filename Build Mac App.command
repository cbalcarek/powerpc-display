#!/bin/sh
# Double-click on your Apple Silicon Mac: builds "PowerPC Display.app",
# installs it in /Applications and opens it.
cd "$(dirname "$0")/host" || exit 1
sh build-app.sh || { echo; echo "Build failed - copy the errors above to Claude."; exit 1; }
pkill -x PowerPCDisplay >/dev/null 2>&1
pkill -x g4host >/dev/null 2>&1          # the old "G4 Display" app
sleep 1
if [ -d "/Applications/G4 Display.app" ]; then
    echo "Removing the old G4 Display app (now PowerPC Display)..."
    rm -rf "/Applications/G4 Display.app"
fi
rm -rf "/Applications/PowerPC Display.app"
mv "build/PowerPC Display.app" /Applications/ && open "/Applications/PowerPC Display.app"
mkdir -p ../releases
ditto -c -k --keepParent "/Applications/PowerPC Display.app" ../releases/PowerPC-Display-Mac.zip &&
    echo "Saved releases/PowerPC-Display-Mac.zip (for GitHub releases)."
echo "Installed and opened /Applications/PowerPC Display.app - you can close this window."

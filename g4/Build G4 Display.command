#!/bin/sh
# G4 Display is now called PowerPC Display. This stub only exists so older
# copies of update.sh keep working; it runs the new build script.
cd "`dirname "$0"`" || exit 1
if [ -f "Build PowerPC Display.command" ]; then
    exec sh "Build PowerPC Display.command"
fi
echo "G4 Display has been renamed PowerPC Display, and update.sh has just updated itself."
echo "Run the same update.sh command once more to get and build PowerPC Display."

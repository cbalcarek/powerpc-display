#!/bin/sh
# Double-click in Tiger (the UTM VM or a real PowerPC Mac): fetches the latest
# source from your Mac and builds the PowerPC app. Your Mac must be running
# "Rebuild Both Apps.command" (or "Share with PowerPC Mac.command").
# The built app goes back to your Mac by itself.
cd "`dirname "$0"`" || exit 1
sh update.sh "$@"

#!/bin/sh
# Double-click on the M1: writes ~/Downloads/g4display.iso with the G4 code,
# for the Tiger VM in UTM.
exec sh "$(dirname "$0")/make-iso.sh"

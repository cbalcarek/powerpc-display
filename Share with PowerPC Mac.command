#!/bin/sh
# Double-click on your Apple Silicon Mac and leave the window open: lets the
# PowerPC Mac download Xcode and the latest PowerPC Display source.
exec sh "$(dirname "$0")/serve.sh"

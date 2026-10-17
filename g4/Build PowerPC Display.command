#!/bin/sh
# Double-click in the Finder (on a G3, G4 or G5 with Tiger, or the Tiger VM)
# to build "PowerPC Display.app" and the command-line ppcdisplay.
# Needs Xcode 2.5 (or any Xcode 2.x for Tiger).
#
# The app gets one copy of the code tuned for each PowerPC generation
# (G3, G4 7400, G4 7450, G5) and Mac OS X runs the right one, so an app built
# here also works, at full speed, on any other PowerPC Mac.
cd "`dirname "$0"`" || exit 1

# Run from the read-only disc? Copy the whole g4display folder home and build there.
if ! touch .ppc_write_test 2>/dev/null; then
    SRC=`cd .. && pwd`
    echo "This folder is read-only (the CD). Copying it to $HOME/g4display ..."
    rm -rf "$HOME/g4display"
    cp -R "$SRC" "$HOME/g4display" || exit 1
    cd "$HOME/g4display/g4" || exit 1
else
    rm -f .ppc_write_test
fi
CODEC="../host/Sources/CCodec"
[ -f "$CODEC/g4codec.c" ] || CODEC="/Volumes/g4display/g4display/host/Sources/CCodec"
if [ ! -f "$CODEC/g4codec.c" ]; then
    echo "Can't find host/Sources/CCodec next to this folder. Copy the whole g4display folder, not just g4."
    exit 1
fi
if ! which gcc >/dev/null 2>&1; then
    echo "gcc not found: install Xcode 2.5 (Developer Tools) first."
    exit 1
fi

THIS_CPU=`machine 2>/dev/null`
case "$THIS_CPU" in
    ppc750)  echo "This Mac: PowerPC G3" ;;
    ppc7400) echo "This Mac: PowerPC G4 (7400/7410)" ;;
    ppc7450) echo "This Mac: PowerPC G4 (7450 family)" ;;
    ppc970)  echo "This Mac: PowerPC G5" ;;
    *)       echo "This Mac: $THIS_CPU" ;;
esac

COMMON="-O3 -fomit-frame-pointer -Wall -I$CODEC/include"
LIBS="-framework OpenGL -framework ApplicationServices -framework Carbon"
TMP="/tmp/ppcdisplay-build.$$"
rm -rf "$TMP"; mkdir -p "$TMP" || exit 1

flags_for() {
    case "$1" in
        ppc750)  echo "-mcpu=750 -mtune=750" ;;
        ppc7400) echo "-mcpu=7400 -mtune=7400" ;;
        ppc7450) echo "-mcpu=7450 -mtune=7450" ;;
        ppc970)  echo "-mcpu=970 -mtune=970" ;;
        *)       echo "-mcpu=750 -mtune=7450" ;;    # generic: runs on every PowerPC Mac
    esac
}

# AltiVec for the codec on G4/G5 (video conversion 8 pixels at a time)
vec_for() {
    case "$1" in ppc7400|ppc7450|ppc970) echo "-maltivec -mabi=altivec" ;; *) echo "" ;; esac
}

# the pairing code (only runs while connecting) is built once, for any PowerPC
echo "Building the pairing code (step 1 of 5)..."
gcc -arch ppc -std=gnu99 $COMMON -O2 -c -o "$TMP/auth.o" "$CODEC/g4auth.c" &&
gcc -arch ppc -std=gnu99 $COMMON -O2 -w -c -o "$TMP/nacl.o" "$CODEC/g4nacl.c" || { rm -rf "$TMP"; exit 1; }

# build one CPU-specific copy of the app's program. $1 = arch, $2 = output
build_slice() {
    A="$1"; F=`flags_for "$1"`; V=`vec_for "$1"`
    gcc -arch $A -std=gnu99 $COMMON $F -DG4_APP -c -o "$TMP/client-$A.o" client.c &&
    { gcc -arch $A -std=gnu99 $COMMON $F $V -c -o "$TMP/codec-$A.o" "$CODEC/g4codec.c" ||
      gcc -arch $A -std=gnu99 $COMMON $F -c -o "$TMP/codec-$A.o" "$CODEC/g4codec.c"; } &&
    gcc -arch $A $COMMON $F -c -o "$TMP/app-$A.o" app.m &&
    gcc -arch $A -o "$2" "$TMP/app-$A.o" "$TMP/client-$A.o" "$TMP/codec-$A.o" "$TMP/auth.o" "$TMP/nacl.o" \
        $LIBS -framework Cocoa
}

APP="PowerPC Display.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
SLICES=""
STEP=2
for A in ppc750 ppc7400 ppc7450 ppc970; do
    echo "Building PowerPC Display.app for $A (step $STEP of 5; each takes a few minutes in a VM)..."
    STEP=`expr $STEP + 1`
    if build_slice $A "$TMP/PowerPCDisplay-$A" 2>"$TMP/err-$A.txt"; then
        SLICES="$SLICES $TMP/PowerPCDisplay-$A"
    else
        echo "  (skipped $A: this Xcode can't build for it)"
        [ "$A" = "$THIS_CPU" ] && cat "$TMP/err-$A.txt"
    fi
done
if [ -n "$SLICES" ] && lipo -create $SLICES -output "$APP/Contents/MacOS/PowerPCDisplay" 2>"$TMP/lipo.txt"; then
    echo "One app for G3, G4 and G5:" `lipo -info "$APP/Contents/MacOS/PowerPCDisplay" | sed 's/.*are: //'`
else
    echo "Couldn't build per-CPU copies; building one generic version that runs on any PowerPC Mac..."
    build_slice ppc "$APP/Contents/MacOS/PowerPCDisplay" || { rm -rf "$TMP"; exit 1; }
fi
rm -rf "$TMP"
[ -f AppIcon.icns ] && cp AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple Computer//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>         <string>local.ppcdisplay.client</string>
    <key>CFBundleName</key>               <string>PowerPC Display</string>
    <key>CFBundleExecutable</key>         <string>PowerPCDisplay</string>
    <key>CFBundleIconFile</key>           <string>AppIcon</string>
    <key>CFBundlePackageType</key>        <string>APPL</string>
    <key>CFBundleSignature</key>          <string>????</string>
    <key>CFBundleShortVersionString</key> <string>0.9</string>
    <key>CFBundleVersion</key>            <string>10</string>
    <key>NSPrincipalClass</key>           <string>NSApplication</string>
    <key>LSMinimumSystemVersion</key>     <string>10.4</string>
</dict>
</plist>
PLIST
echo 'APPL????' > "$APP/Contents/PkgInfo"
touch "$APP"   # make the Finder notice the new icon

# Retire the old "G4 Display" app (renamed to PowerPC Display)
killall G4Display PowerPCDisplay >/dev/null 2>&1
rm -rf "G4 Display.app" g4display
if [ -d "/Applications/G4 Display.app" ]; then
    echo "Removing the old G4 Display app..."
    rm -rf "/Applications/G4 Display.app"
    osascript -e 'tell application "System Events" to if exists login item "G4 Display" then delete login item "G4 Display"' >/dev/null 2>&1
fi

# zip for a GitHub release, and hand it to the Apple Silicon Mac if we know where it is
ZIP="PowerPC-Display-Tiger.zip"
rm -f "$ZIP"
if ditto -c -k --keepParent "$APP" "$ZIP" 2>/dev/null; then
    # where the Apple Silicon Mac might be: remembered by update.sh, else the
    # addresses make-iso.sh put on the disc (UTM VM addresses + its name)
    SENT=""
    HOSTS=""
    [ -f .m1host ] && HOSTS="`cat .m1host`"
    [ -f .m1hosts ] && HOSTS="$HOSTS `cat .m1hosts`"
    for h in $HOSTS; do
        if curl -fsS --connect-timeout 3 -m 60 -T "$ZIP" "http://$h:8000/releases/$ZIP" 2>/dev/null; then
            SENT=$h; break
        fi
    done
    if [ -n "$SENT" ]; then
        echo "Sent $ZIP to your Mac at $SENT (g4display/releases)."
    else
        echo "Made $ZIP, but couldn't reach your Mac to send it."
        echo "Is Share with PowerPC Mac.command running there? To send it by hand:"
        echo "  curl -T \"`pwd`/$ZIP\" http://YOUR-MAC.local:8000/releases/$ZIP"
    fi
fi

echo
echo "Built: `pwd`/$APP"
if rm -rf "/Applications/$APP" 2>/dev/null && cp -R "$APP" /Applications/ 2>/dev/null; then
    echo "Installed in Applications. Opening it..."
    open "/Applications/$APP"
else
    echo "Couldn't copy it into Applications; opening it from here instead."
    open "$APP"
fi

# the command-line ppcdisplay (debugging, -t test mode): last, so the app is
# ready first. Skip it with: NO_CLI=1 "./Build PowerPC Display.command"
if [ -z "$NO_CLI" ]; then
    echo
    echo "Also building the command-line ppcdisplay for this Mac (optional; you can close this window)..."
    case "$THIS_CPU" in ppc750|ppc7400|ppc7450|ppc970) CLI_ARCH=$THIS_CPU ;; *) CLI_ARCH=ppc ;; esac
    gcc -arch $CLI_ARCH -std=gnu99 $COMMON `flags_for $CLI_ARCH` `vec_for $CLI_ARCH` -o ppcdisplay client.c "$CODEC/g4codec.c" "$CODEC/g4auth.c" "$CODEC/g4nacl.c" $LIBS 2>/dev/null ||
    gcc -std=gnu99 $COMMON `flags_for ppc` -o ppcdisplay client.c "$CODEC/g4codec.c" "$CODEC/g4auth.c" "$CODEC/g4nacl.c" $LIBS 2>/dev/null &&
    echo "Built: `pwd`/ppcdisplay"
fi
echo "Done."

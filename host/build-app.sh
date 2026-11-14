#!/bin/sh
# Builds PowerPC Display.app (window + menu bar app) from this Swift package,
# Universal: Apple Silicon and Intel in one app, each Mac runs its own half.
# Usage: ./build-app.sh            -> build/PowerPC Display.app
#        ARCHS=arm64 ./build-app.sh  (one kind of Mac only: faster to build)
set -e
cd "$(dirname "$0")"

# Each architecture separately, then glued together with lipo. (One
# "swift build --arch arm64 --arch x86_64" needs the full Xcode; this works
# with just the Command Line Tools too.)
ARCHS="${ARCHS:-arm64 x86_64}"
MINOS=12.3              # oldest macOS it runs on (ScreenCaptureKit); also LSMinimumSystemVersion below
OUT="$(mktemp -d)"
SLICES=""
for A in $ARCHS; do
    echo "Building for $A..."
    # its own build folder per architecture (newer Xcodes put every
    # architecture's result in the same place), and an explicit target so
    # the minimum macOS stays $MINOS whatever Xcode defaults to
    SCRATCH=".build/$A"
    if swift build -c release --scratch-path "$SCRATCH" --triple "$A-apple-macosx$MINOS"; then
        B="$(swift build -c release --scratch-path "$SCRATCH" --triple "$A-apple-macosx$MINOS" --show-bin-path)/PowerPCDisplay"
        if [ "$(lipo -archs "$B" 2>/dev/null)" != "$A" ]; then
            echo "The $A build produced $(lipo -archs "$B" 2>/dev/null) instead."
            exit 1
        fi
        cp "$B" "$OUT/PowerPCDisplay-$A"
        SLICES="$SLICES $OUT/PowerPCDisplay-$A"
    elif [ "$A" = x86_64 ] && [ -n "$SLICES" ]; then
        echo "WARNING: the Intel build failed (errors above), so this app is Apple Silicon only."
    else
        exit 1
    fi
done
BIN="$OUT/PowerPCDisplay"
# shellcheck disable=SC2086
lipo -create $SLICES -output "$BIN"
echo "Built for: $(lipo -archs "$BIN")"
MIN_BUILT="$(vtool -show-build "$BIN" 2>/dev/null | awk '/minos/ { print $2; exit }')"
[ -n "$MIN_BUILT" ] && echo "Runs on macOS $MIN_BUILT and later."
if [ -n "$MIN_BUILT" ] && [ "$MIN_BUILT" != "$MINOS" ]; then
    echo "WARNING: expected macOS $MINOS as the minimum, got $MIN_BUILT."
fi

APP="build/PowerPC Display.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/PowerPCDisplay"
[ -f ../icon/AppIcon.icns ] && cp ../icon/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"
# the PowerPC app built in the VM, so PowerPC Macs can update from this Mac
if [ -f ../releases/PowerPC-Display-PPC.zip ]; then
    cp ../releases/PowerPC-Display-PPC.zip "$APP/Contents/Resources/"
    echo "Included releases/PowerPC-Display-PPC.zip (PowerPC Macs will be offered it)."
fi

# macOS 26 frames app icons in grey unless they come from Xcode's Icon
# Composer format. Compile icon/AppIcon.icon with Xcode's actool if it's
# installed (any Xcode 26 in /Applications); otherwise keep the .icns above.
ICON_NAME_KEY=""
if [ -d ../icon/AppIcon.icon ]; then
    ACTOOL_OK=""
    for DEV in "" /Applications/Xcode*.app/Contents/Developer; do
        [ -n "$DEV" ] && [ ! -d "$DEV" ] && continue
        TMPI="$(mktemp -d)"
        if DEVELOPER_DIR="${DEV:-$(xcode-select -p 2>/dev/null)}" xcrun actool ../icon/AppIcon.icon \
               --compile "$TMPI" --platform macosx --minimum-deployment-target "$MINOS" \
               --app-icon AppIcon --output-partial-info-plist "$TMPI/partial.plist" \
               --output-format human-readable-text >"$TMPI/log.txt" 2>&1 \
           && [ -f "$TMPI/Assets.car" ]; then
            cp "$TMPI/Assets.car" "$APP/Contents/Resources/"
            [ -f "$TMPI/AppIcon.icns" ] && cp "$TMPI/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
            ICON_NAME_KEY="    <key>CFBundleIconName</key>           <string>AppIcon</string>"
            ACTOOL_OK=1
            echo "App icon compiled with Xcode's actool (macOS 26 style)."
            rm -rf "$TMPI"
            break
        fi
        LASTLOG="$(cat "$TMPI/log.txt" 2>/dev/null | head -5)"
        rm -rf "$TMPI"
    done
    if [ -z "$ACTOOL_OK" ]; then
        echo "Note: no Xcode 26 actool found, so macOS 26 may show the icon in a grey frame."
        [ -n "$LASTLOG" ] && echo "$LASTLOG"
    fi
fi

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleIdentifier</key>         <string>local.ppcdisplay.host</string>
    <key>CFBundleName</key>               <string>PowerPC Display</string>
    <key>CFBundleDisplayName</key>        <string>PowerPC Display</string>
    <key>CFBundleExecutable</key>         <string>PowerPCDisplay</string>
    <key>CFBundleIconFile</key>           <string>AppIcon</string>
$ICON_NAME_KEY
    <key>CFBundlePackageType</key>        <string>APPL</string>
    <key>CFBundleShortVersionString</key> <string>0.14</string>
    <key>CFBundleVersion</key>            <string>14</string>
    <key>LSMinimumSystemVersion</key>     <string>12.3</string>
    <!-- macOS local-network privacy: required to find the G4 with Bonjour -->
    <key>NSAudioCaptureUsageDescription</key>
    <string>PowerPC Display plays this Mac's sound on a PowerPC Mac, and mutes it here meanwhile.</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>PowerPC Display finds and streams to your PowerPC Mac on the local network.</string>
    <key>NSBonjourServices</key>
    <array><string>_g4display._tcp</string></array>
</dict>
</plist>
PLIST

# Sign with a stable local certificate, so macOS keeps the Screen Recording
# and Local Network permissions across rebuilds (an ad-hoc signature changes
# on every build, which silently invalidates them).
KC="$HOME/Library/Keychains/login.keychain-db"
IDENTITY="PowerPC Display Local Signing"
has_identity() { security find-identity -p codesigning "$KC" 2>/dev/null | grep -q "\"$1\""; }
# reuse the certificate made by earlier versions (called G4 Display) if it works
has_identity "G4 Display Local Signing" && IDENTITY="G4 Display Local Signing"

if ! has_identity "$IDENTITY"; then
    echo "Creating a local code-signing certificate \"$IDENTITY\" (one time)..."
    TMPD="$(mktemp -d)"
    cat > "$TMPD/cert.cnf" <<CNF
[req]
distinguished_name = dn
x509_extensions = ext
prompt = no
[dn]
CN = $IDENTITY
[ext]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
CNF
    # Apple's own openssl (LibreSSL), not Homebrew's: OpenSSL 3 writes .p12
    # files the macOS keychain can't import
    OSSL=/usr/bin/openssl
    if $OSSL req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$TMPD/cert.cnf" \
           -keyout "$TMPD/key.pem" -out "$TMPD/cert.pem" \
       && $OSSL pkcs12 -export -inkey "$TMPD/key.pem" -in "$TMPD/cert.pem" -name "$IDENTITY" \
           -passout pass:ppcdisplay -out "$TMPD/id.p12" \
       && security import "$TMPD/id.p12" -k "$KC" -P ppcdisplay -T /usr/bin/codesign; then
        echo "Created \"$IDENTITY\" in your login keychain."
    else
        echo "Couldn't create the signing certificate (errors above)."
    fi
    rm -rf "$TMPD"
fi

sign_with_identity() { codesign --force --sign "$IDENTITY" --keychain "$KC" "$APP"; }
SIGNED=""
if has_identity "$IDENTITY"; then
    if sign_with_identity; then
        SIGNED=1
    else
        # some macOS versions only sign with a certificate trusted for code signing
        echo "Trusting \"$IDENTITY\" for code signing (macOS may ask for your password once)..."
        TMPC="$(mktemp -d)"
        security find-certificate -c "$IDENTITY" -p "$KC" > "$TMPC/cert.pem" \
            && security add-trusted-cert -r trustRoot -p codeSign -k "$KC" "$TMPC/cert.pem" \
            && sign_with_identity && SIGNED=1
        rm -rf "$TMPC"
    fi
fi
if [ -n "$SIGNED" ]; then
    echo "Signed with \"$IDENTITY\" (permissions will survive rebuilds)."
else
    codesign --force --sign - "$APP"
    echo "WARNING: signed ad hoc. macOS will ask for Screen Recording again after every rebuild."
fi

echo
echo "Built: $(pwd)/$APP"
touch "$APP"   # make Finder pick up the new icon
echo "Move it to /Applications and open it: a PowerPC Display window opens, plus a menu bar icon."

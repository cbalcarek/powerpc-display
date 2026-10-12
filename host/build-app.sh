#!/bin/sh
# Builds PowerPC Display.app (window + menu bar app) from this Swift package.
# Usage: ./build-app.sh            -> build/PowerPC Display.app
set -e
cd "$(dirname "$0")"

swift build -c release
BIN="$(swift build -c release --show-bin-path)/PowerPCDisplay"

APP="build/PowerPC Display.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN" "$APP/Contents/MacOS/PowerPCDisplay"
[ -f ../icon/AppIcon.icns ] && cp ../icon/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

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
               --compile "$TMPI" --platform macosx --minimum-deployment-target 14.0 \
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
    <key>CFBundleShortVersionString</key> <string>0.8</string>
    <key>CFBundleVersion</key>            <string>8</string>
    <key>LSMinimumSystemVersion</key>     <string>14.0</string>
    <!-- macOS local-network privacy: required to find the G4 with Bonjour -->
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
IDENTITY="PowerPC Display Local Signing"
# reuse the certificate made by earlier versions (called G4 Display) if present
security find-certificate -c "G4 Display Local Signing" >/dev/null 2>&1 && IDENTITY="G4 Display Local Signing"
if ! security find-identity -p codesigning 2>/dev/null | grep -q "$IDENTITY" \
   && ! security find-certificate -c "$IDENTITY" >/dev/null 2>&1; then
    echo "Creating a local signing certificate (one time)..."
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
    if openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -config "$TMPD/cert.cnf" \
           -keyout "$TMPD/key.pem" -out "$TMPD/cert.pem" >/dev/null 2>&1 \
       && openssl pkcs12 -export -inkey "$TMPD/key.pem" -in "$TMPD/cert.pem" \
           -name "$IDENTITY" -passout pass:g4display -out "$TMPD/id.p12" >/dev/null 2>&1 \
       && security import "$TMPD/id.p12" -k "$HOME/Library/Keychains/login.keychain-db" \
           -P g4display -T /usr/bin/codesign >/dev/null; then
        echo "Created \"$IDENTITY\" in your login keychain."
    else
        echo "Couldn't create a signing certificate; using an ad-hoc signature."
    fi
    rm -rf "$TMPD"
fi
if security find-certificate -c "$IDENTITY" >/dev/null 2>&1 \
   && codesign --force --sign "$IDENTITY" "$APP" 2>/dev/null; then
    echo "Signed with \"$IDENTITY\" (permissions will survive rebuilds)."
else
    codesign --force --sign - "$APP"
    echo "Signed ad hoc: after this rebuild, re-allow Screen Recording for PowerPC Display."
fi

echo
echo "Built: $(pwd)/$APP"
touch "$APP"   # make Finder pick up the new icon
echo "Move it to /Applications and open it: a PowerPC Display window opens, plus a menu bar icon."

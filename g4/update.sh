#!/bin/sh
# Fetch the latest PowerPC Display source from your Apple Silicon Mac and
# rebuild. Run on the PowerPC Mac (or the Tiger VM) from anywhere:
#
#   sh update.sh                      # UTM VM: tries 10.0.2.2, then 192.168.64.1
#   sh update.sh Colins-iMac-2.local  # a real PowerPC Mac: your Mac's name
#
# The address you use is remembered for next time.
# Your Mac must be running "Share with PowerPC Mac.command" (serve.sh).

cd "`dirname "$0"`" || exit 1          # .../g4display/g4
mkdir -p ../host/Sources/CCodec/include

if [ -n "$1" ]; then
    CANDIDATES="$1"
elif [ -f .m1host ]; then
    CANDIDATES="`cat .m1host` 10.0.2.2 192.168.64.1"
else
    CANDIDATES="10.0.2.2 192.168.64.1"
fi
M1=""
for h in $CANDIDATES; do
    if curl -fsS -m 4 -o /dev/null "http://$h:8000/g4/update.sh" 2>/dev/null; then M1=$h; break; fi
done
if [ -z "$M1" ]; then
    echo "Can't reach your Mac at: $CANDIDATES"
    echo "Is \"Share with PowerPC Mac.command\" running on it? On a real PowerPC Mac, pass your Mac's name:"
    echo "  sh update.sh Your-Mac-Name.local"
    exit 1
fi
echo "$M1" > .m1host
BASE="http://$M1:8000"
echo "Getting the latest source from $M1..."

for f in g4/client.c g4/app.m g4/g4client.h g4/AppIcon.icns "g4/Build PowerPC Display.command" \
         "g4/Build G4 Display.command" \
         host/Sources/CCodec/g4codec.c host/Sources/CCodec/include/g4codec.h \
         host/Sources/CCodec/g4auth.c host/Sources/CCodec/include/g4auth.h \
         host/Sources/CCodec/g4nacl.c host/Sources/CCodec/g4nacl.h g4/update.sh; do
    out="../$f"
    [ "$f" = g4/update.sh ] && out="../$f.new"    # don't overwrite this script while it runs
    url=`echo "$f" | sed 's/ /%20/g'`
    if ! curl -fsS -o "$out" "$BASE/$url"; then
        echo "Could not download $f from $BASE"
        exit 1
    fi
done
mv -f update.sh.new update.sh
chmod +x *.command update.sh

sh "Build PowerPC Display.command"

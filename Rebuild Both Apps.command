#!/bin/sh
# Double-click on your Mac to rebuild both apps:
#   1. shares this folder (like Share with PowerPC Mac.command)
#   2. waits while you double-click "Get Latest and Build.command" in Tiger
#      (in ~/g4display/g4); the built PowerPC app comes back here by itself
#   3. stops sharing and builds the Mac app (with the new PowerPC app inside),
#      installs it and opens it
# Close this window to stop.
cd "$(dirname "$0")" || exit 1
mkdir -p releases
ZIP=releases/PowerPC-Display-PPC.zip
MARK="$(mktemp)"                       # anything newer than this is the new build

python3 - >/tmp/ppcdisplay-share.log 2>&1 <<'PY' &
import http.server, os, re
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_PUT(self):
        m = re.fullmatch(r"/releases/([A-Za-z0-9._-]+\.zip)", self.path)
        n = int(self.headers.get("Content-Length", "0"))
        if not m or n <= 0 or n > 200 * 1024 * 1024:
            self.send_error(403); return
        data = self.rfile.read(n)
        tmp = os.path.join("releases", "." + m.group(1) + ".part")
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, os.path.join("releases", m.group(1)))
        self.send_response(201); self.end_headers()
http.server.ThreadingHTTPServer(("0.0.0.0", 8000), H).serve_forever()
PY
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -f "$MARK"' EXIT INT TERM HUP
sleep 1
if ! kill -0 $SERVER 2>/dev/null; then
    echo "Couldn't start sharing on port 8000. Is Share with PowerPC Mac.command already"
    echo "running? Close it and try again."
    cat /tmp/ppcdisplay-share.log
    exit 1
fi

echo "Step 1 of 2: sharing this folder with the Tiger VM."
echo
echo "  In Tiger, double-click  Get Latest and Build.command  in your g4display/g4 folder"
echo "  (in your home folder). It takes a few minutes in the VM."
echo
echo "  If Tiger doesn't have it yet, open Terminal there and paste this once:"
echo "    sh ~/g4display/g4/update.sh 10.0.2.2"
echo
echo "Waiting for the PowerPC app to come back (close this window to stop)..."
while [ ! "$ZIP" -nt "$MARK" ]; do sleep 2; done
echo "Received $ZIP ($(( $(stat -f %z "$ZIP") / 1024 )) KB)."
kill $SERVER 2>/dev/null

echo
echo "Step 2 of 2: building the Mac app..."
sh "Build Mac App.command"

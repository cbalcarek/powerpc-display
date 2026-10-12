#!/bin/sh
# Run on your Apple Silicon Mac and leave it running: shares this folder so a
# PowerPC Mac (or the Tiger VM) can fetch the latest source with g4/update.sh,
# and receives the built Tiger app into releases/ (for GitHub releases).
cd "$(dirname "$0")"
mkdir -p releases
echo "Serving $(pwd) on port 8000 — press Control-C to stop."
NAME="$(scutil --get LocalHostName 2>/dev/null || hostname -s).local"
echo
echo "On the PowerPC Mac (G3, G4 or G5), in Terminal:"
[ -e xcode.dmg ] && echo "  Xcode installer:  curl -O http://$NAME:8000/xcode.dmg && open xcode.dmg"
echo "  PowerPC Display:  mkdir -p ~/g4display/g4 && cd ~/g4display/g4 && curl -O http://$NAME:8000/g4/update.sh && sh update.sh $NAME"
echo "  Later updates:    sh ~/g4display/g4/update.sh $NAME"
echo
exec python3 - <<'PY'
import http.server, os, re
class H(http.server.SimpleHTTPRequestHandler):
    # PUT /releases/<name>.zip  -> saves into ./releases (nothing else is writable)
    def do_PUT(self):
        m = re.fullmatch(r"/releases/([A-Za-z0-9._-]+\.zip)", self.path)
        n = int(self.headers.get("Content-Length", "0"))
        if not m or n <= 0 or n > 200 * 1024 * 1024:
            self.send_error(403); return
        data = self.rfile.read(n)
        with open(os.path.join("releases", m.group(1)), "wb") as f:
            f.write(data)
        print(f"received releases/{m.group(1)} ({n // 1024} KB)")
        self.send_response(201); self.end_headers()
http.server.ThreadingHTTPServer(("0.0.0.0", 8000), H).serve_forever()
PY

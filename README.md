# PowerPC Display — a G3, G4 or G5 Mac as a display for a modern Mac

(Formerly "G4 Display". The project folder is still called `g4display`.)

The modern Mac can be Apple Silicon or Intel, with macOS 14 or later: the
Mac app is Universal. This README says "Apple Silicon Mac" for it either way.

The Apple Silicon Mac creates a virtual monitor the same size as the PowerPC
Mac's screen, captures it with ScreenCaptureKit, and streams only what changed
over Ethernet. The PowerPC Mac (Mac OS X 10.4 Tiger) shows it full screen.
When the Apple Silicon Mac disconnects, the PowerPC Mac gets its own desktop
back. No hardware modification.

## Which PowerPC Macs

Any Mac that runs Tiger: G3 (with built-in FireWire), G4 or G5. One build of
the app contains code tuned for each chip (G3, G4 7400, G4 7450, G5) and Mac
OS X runs the right one, so an app built in the Tiger VM also works on any
real PowerPC Mac.

It adapts to the machine automatically:

| | What it does |
|---|---|
| Screen size | The virtual display matches the PowerPC Mac's current resolution and physical size, and is named after it in System Settings › Displays. Set the PowerPC Mac to its panel's native resolution first. |
| Colour | **Automatic**: Thousands (16-bit) on a G3, when the screen is set to Thousands, or with too little video memory for 32-bit; Millions (32-bit) otherwise. Can be overridden in the app. |
| Tile cache | Sized from RAM: 512 tiles up to 256 MB, 1024 up to 512 MB, 2048 up to 1 GB, 4096 above. |
| Drawing | OpenGL texture fast path when the GPU supports it; otherwise straight into the framebuffer (old ATI Rage chips, very large screens such as a 30" display on a Radeon 9600, emulators); Apple's software OpenGL as a last resort. |

Expect the G3 to manage fewer frames per second when a lot of the screen
changes (no AltiVec, slower memory bus); typing, scrolling and the pointer
stay responsive. A G5 is the fastest.

```
Apple Silicon Mac ────────────────────────────────────────►  PowerPC Mac (Tiger)
virtual display → ScreenCaptureKit → diff vs. its model   →   decode into texture memory
  scroll detection (COPY) · solid fills · LZ4                  → DMA of only the changed bands
  video regions as 4:2:2 YCbCr (optional)
cursor position @125 Hz (tiny messages)                        → cursor drawn locally
                      ◄──────── ACK after each frame is on screen
```

## Why it should feel better than VNC

| Problem in VNC | What g4display does |
|---|---|
| Client must poll for updates | Host pushes the moment the screen changes |
| Cursor baked into the image, so it lags | The PowerPC Mac draws the cursor itself from 8-byte position messages |
| Scrolling resends the whole window | Detects vertical scrolls and sends a COPY + only the new lines |
| Generic pixel formats the client must convert | Pixels arrive in the GPU's native big-endian layout; zero conversion |
| Copies into the GPU | Framebuffer is the texture's backing store (Apple client storage + texture range), kept in VRAM |
| Buffers fill up, lag grows | ≤2 frames in flight; changes coalesce on the Apple Silicon Mac while the PowerPC Mac catches up |
| Repainting things it has already seen | Tile cache: menus closing, switching apps back, caret blink cost 12 bytes per 32×32 tile |

Measured on the test desktop (1440×900, 32-bit):

| Scenario | Bytes sent per frame | 100 Mbps transfer |
|---|---|---|
| Typing (one line changes) | ~10 KB | ~1 ms |
| Scrolling code 1 line | ~3 KB | <1 ms |
| Scrolling code 3 lines | ~29 KB | ~2 ms |
| Caret blink | 0.5 KB (was 11 KB without cache) | ~0 ms |
| Menu open/close | ~12 KB (was 57 KB) | ~1 ms |
| Switching between two apps | ~25 KB (was 254 KB) | ~2 ms |
| Whole screen changes to new content | ~220 KB | ~18 ms |
| First frame | ~440 KB | ~35 ms |

Thousands (16-bit) colour sends roughly a third less data and halves the
memory traffic (slight banding on gradients; text is fine).

## Pairing (once per pair of Macs)

The first time a PowerPC Mac and an Apple Silicon Mac meet, both show the
same 6-digit code: check they match and click **Pair** on both. After that
they connect without asking. Only paired Macs can receive your screen or
drive the PowerPC Mac's display, so it's safe on a shared network too.

How it works: both Macs make a fresh X25519 key pair and derive the same
secret key and code from it (if anyone in between tampered with the
exchange, the codes wouldn't match). Every later connection, each side proves
it holds the key (HMAC-SHA512 over fresh random numbers from both sides)
before any screen data is sent. Keys are in the Apple Silicon Mac's login
keychain and in `~/Library/Application Support/PowerPC Display/pairing` on
the PowerPC Mac. Screen data itself isn't encrypted (too costly for a G3/G4);
pairing controls who can get it.

To start over: **Forget Paired PowerPC Macs…** (app menu) on the
Apple Silicon Mac, **Forget Paired Macs…** in the app menu on the PowerPC Mac.

Both apps need this version: an older PowerPC app is told to update.

## 1. Network

Plug an Ethernet cable from a USB-C Ethernet adapter on the Apple Silicon Mac
straight into the PowerPC Mac (most USB-C adapters handle the crossover automatically; if the link
light stays off, go through a switch). That's it: both Macs give themselves
self-assigned addresses and Bonjour finds the PowerPC Mac, so no IP
addresses need to be set. Both on your home network works too; a direct cable
just avoids Wi-Fi jitter.

If Tiger's firewall is on (System Preferences › Sharing › Firewall), allow TCP
port 5960.

## 2. The PowerPC Mac (Mac OS X 10.4)

Needs Xcode 2.5 (free from developer.apple.com › More Downloads; only
"Developer Tools Software" and the 10.4 SDK are needed).

1. Get the `g4display` folder onto it: from the UTM disc made by
   **Make ISO for UTM.command**, or over the network with `g4/update.sh`
   (see "Updating" below). Copy the whole folder, not just `g4`.
2. Double-click **g4/Build PowerPC Display.command**. Terminal opens, builds
   (a few minutes: once per chip type), installs **PowerPC Display.app** in
   Applications and opens it. It also removes the old G4 Display app.

A small window shows what it's doing ("Waiting for your Mac … this Mac shows
up as …"), which chip, RAM and video memory it found, a **Colour** menu
(Automatic / Millions / Thousands) and **Open PowerPC Display at login**.
When the other Mac connects, the PowerPC Mac takes over the whole screen (menu
bar and Dock hidden) and stays awake while connected. **Command-Escape**
pauses and brings the window back; click **Resume** to let it reconnect.

Optional, once: click **Fix Network Stalls…** in the window (asks for an
administrator password). Tiger's default TCP delayed-acknowledgement setting
(3) can stall bursts of data on 10.4; this sets the commonly recommended 2,
now and after restarts. The button shows "Network tuned" once it's set
(0 counts too). By hand, in Terminal:

```sh
sudo sysctl -w net.inet.tcp.delayed_ack=2
echo net.inet.tcp.delayed_ack=2 | sudo tee -a /etc/sysctl.conf   # keep after reboot
```

The build also makes a command-line `ppcdisplay` (for the chip it was built
on) with extra options for debugging: `-v` (stats), `-16` / `-32`, `-c N`
(cache tiles), `-t` (test mode: decodes frames without drawing and saves
`~/Desktop/ppcdisplay-test.png`). It prints the chip and automatic choices
when it starts.

## 3. The Apple Silicon (or Intel) Mac (macOS 14+)

Double-click **Build Mac App.command** (in the `g4display` folder). It builds
PowerPC Display.app, installs it in /Applications, removes the old G4 Display
app and opens it. (Same thing by hand: `cd host && sh build-app.sh`, then move
`build/PowerPC Display.app` to /Applications.)

The app is Universal: it's built twice, for Apple Silicon and for Intel, and
each Mac runs its own half at full speed. Build it on an Apple Silicon Mac.
`ARCHS=arm64 sh build-app.sh` builds just the Apple Silicon half (quicker
while trying things out). If the Intel half fails to build, the script says
so and makes an Apple Silicon-only app. To try the Intel half on an Apple
Silicon Mac (it runs under Rosetta, slower than on a real Intel Mac): `arch
-x86_64 "/Applications/PowerPC Display.app/Contents/MacOS/PowerPCDisplay"`.

A **PowerPC Display**
window opens (clicking the Dock icon brings it back), and there's a matching
menu bar icon. The first launch asks for **Screen Recording** permission and,
the first time it looks for the PowerPC Mac, **Local Network** access; allow both,
then quit and reopen it.

The window is laid out like Bluetooth in System Settings:

- **My PowerPC Macs**: the ones connected (or pairing), with a green dot.
  **Other PowerPC Macs**: ones it knows that aren't connected right now,
  with when they were last connected.
- Click a Mac for its details: live frame rate, KB/s, latency and video
  quality, and what you can do with it:
  - **Show: Extend / Mirror**, per Mac (remembered). Extend adds it as
    another display; Mirror shows your main display on it, scaled down and
    letterboxed to fit.
  - **Arrange Displays…** opens System Settings › Displays to place it.
  - **Disconnect / Connect**: disconnecting also removes its display.
  - **Wake** (a Mac that's asleep and has connected before) sends it
    Wake-on-LAN. **Remove from List…** stops looking for it.
  The same actions are on each row's right-click menu.
- **Video**: frame rate (60 / 30 / 20 / 15 fps, remembered), **Smoother
  video** (on by default: areas that change at video rate are sent as 4:2:2
  YCbCr, half the data, with slightly softer colour in the video; text and
  still areas are always exact, and a video that stops is resent losslessly
  within about a third of a second; needs Millions of colours on the PowerPC
  Mac) and **Match video quality to the network**.
- **General**: **Match this Mac's brightness**, **Launch at login**.
- **Check for Updates…** at the bottom.

The menu bar menu works like the Wi-Fi menu: a line per PowerPC Mac with its
state (✓ when connected), and a submenu for each with the same per-Mac
actions, then the video and brightness switches.

Run the app rather than the bare binary from Xcode: without the app bundle,
macOS can block Bonjour (Local Network privacy) and Launch at Login is
unavailable.

The app is signed with a local certificate the build creates once, so
permissions survive rebuilds. Because the rename changed the app's identity,
macOS asks for Screen Recording and Local Network once more for PowerPC
Display; you can remove the old "G4 Display" entries from those lists.

### Command line (for debugging)

The same binary runs from Terminal and logs to the console:

```sh
"/Applications/PowerPC Display.app/Contents/MacOS/PowerPCDisplay" -v
```

Finding the PowerPC Mac: the app remembers the address that last worked
(also after quitting) and tries it first, so it reconnects even if Bonjour is
having a bad day; Bonjour is used when that address doesn't answer. It also
tries every PowerPC Mac Bonjour lists, so a Tiger VM that was shut down
(which can linger in the list for an hour) doesn't block a real one.

On the PowerPC Mac, the app checks every minute that Bonjour still answers
for it and registers again if not. If Bonjour stays stuck the window says
"Bonjour isn't responding" with a **Restart Bonjour…** button (asks for an
administrator password; same as `sudo killall mDNSResponder` in Terminal).

More than one PowerPC Mac on the network: `--name "iMac G4"` (its computer
name). Bonjour blocked: pass its IP address as the first argument.

Already using BetterDisplay? `PowerPCDisplay --list` shows display IDs, and
`PowerPCDisplay --display-id <id>` captures that display instead of creating
one (make it the PowerPC Mac's resolution, non-HiDPI).

## Several PowerPC Macs at once

Every PowerPC Mac running PowerPC Display that the Apple Silicon Mac finds
becomes its own extra display (or mirror), each with its own row in the
status window and menu, and its own Extend/Mirror choice. They're
remembered, so a known one reconnects at its last address even when Bonjour
isn't answering. **Remove from List…** (for one that isn't connected) stops
looking for it, e.g. an old VM; it comes back if it appears on the network
again.

## A Mac with no monitor (headless)

A Mac mini (or any Mac with no monitor) can use a PowerPC Mac as its only
screen. **Use as this Mac's main display** (General, or the menu bar menu)
does this, and it turns on by itself when no monitor is connected:

- the PowerPC Mac's display is created as soon as PowerPC Display opens, at
  the size it had last time, so it's there before the PowerPC Mac connects
  and stays while it's away (windows don't get moved around)
- it becomes the main display (menu bar and Dock). With several PowerPC
  Macs, the first one to appear is the main display and the others extend it
- Mirror is off (there's no other screen to mirror)
- disconnecting the only display asks first, since you'd need a monitor or
  Screen Sharing to get it back

**Pairing with no screen.** Normally both Macs show a code and you click
Pair on each. A Mac with no screen can't show its code, so the person at
the PowerPC Mac decides alone: its window says a Mac with no screen wants to
use it, and you click **Pair** there. So that a stranger on the network
can't pair with it later, this only works while nothing is paired yet, or
within 10 minutes of PowerPC Display opening (quit and reopen it, or restart
the Mac, to pair another one). When a PowerPC Mac is already showing the
screen, pairing asks on that screen as usual.

**Setting up a Mac mini once:**

1. Turn on **automatic login** (System Settings › Users & Groups). PowerPC
   Display starts at login, so nothing shows before that; automatic login
   needs FileVault off. Startup and the login screen never appear on the
   PowerPC Mac.
2. Once, with a monitor or Screen Sharing: open PowerPC Display, allow
   **Screen Recording** and **Local Network**, and turn on **Launch at login**.
3. Use the mini's own keyboard and mouse: the PowerPC Mac only shows the
   picture.

## Brightness

**Match this Mac's brightness** (on by default): PowerPC Macs with a
built-in screen (iMac G4, iBook, PowerBook; Apple's flat panels too)
follow the Apple Silicon Mac's brightness while they show it, and go back
to their own when you disconnect. The Apple Silicon side reads its
brightness with macOS's DisplayServices, which isn't public: if a future
macOS changes it, brightness matching simply stops.

## Updates

- **Apple Silicon Mac:** **Check for Updates…** (app menu or menu bar
  menu; also checked quietly once a day) looks at this repository's GitHub
  releases, so the repository has to be public. A newer Mac app is
  downloaded, checked (it must be signed by the same certificate as the one
  you have, or it asks first), swapped in and reopened.
- **PowerPC Macs:** they can't reach GitHub themselves (Tiger is too old
  for its secure connections), so the Apple Silicon Mac keeps the newest
  Tiger app (from the GitHub release, from `releases/` after a Tiger build,
  or built into the Mac app) and offers it when an older PowerPC app
  connects. It arrives in the background with a checksum; **Install and
  Restart** appears when you're back at the PowerPC Mac's window (or
  **PowerPC Display › Install Update…**).

## Updating the PowerPC side (no discs, no zips)

On the Apple Silicon Mac, double-click **Share with PowerPC Mac.command** and
leave its window open. Then on the PowerPC Mac or in the Tiger VM, one command
fetches the latest source and rebuilds:

```sh
sh ~/g4display/g4/update.sh                       # in a UTM VM (tries 10.0.2.2 and 192.168.64.1)
sh ~/g4display/g4/update.sh Colins-iMac-2.local   # on a real PowerPC Mac
```

The address that worked is remembered, so afterwards plain
`sh ~/g4display/g4/update.sh` is enough. The first time, get the script
itself with the command the Share window prints.

## If the cable is pulled (or a Mac sleeps)

The Apple Silicon Mac pings every second. If either side hears nothing for
4 seconds it disconnects: the PowerPC Mac goes back to its own desktop, and
the Apple Silicon Mac waits and reconnects when it's back.

## Sleeping and waking with your Mac

When the Apple Silicon Mac goes to sleep, or its displays do, it tells the
PowerPC Mac, which gives its screen back and goes to sleep too. On wake the
Apple Silicon Mac sends it a Wake-on-LAN packet and reconnects; the windows
on the PowerPC screen stay where they were. For the wake-up, turn on
**System Preferences › Energy Saver › Options › Wake for Ethernet network
administrator access** on the PowerPC Mac (the app reminds you once). Not
every G3 has that option; then wake it with its keyboard or mouse and it
reconnects by itself.

Closing a laptop's lid counts as sleep too. To macOS the PowerPC Mac's
screen is an external monitor, which would otherwise keep a laptop awake in
clamshell mode, so with the lid closed its display is removed and the Mac
sleeps as usual. (With a real external monitor attached, the lid behaves as
normal and the PowerPC Macs stay on.) Turn the whole thing off on the PowerPC Mac with
**PowerPC Display › Sleep When Your Mac Sleeps**.

## Logs

- Apple Silicon Mac: Console.app, search for `local.ppcdisplay.host` (or
  "PowerPC Display"); also printed when run from Terminal.
- PowerPC Mac: `~/Library/Logs/PowerPC Display.log` (previous run in
  `.log.old`); open it with Console.app on Tiger.

## Releases

Each build also saves a zip for GitHub releases in `releases/` (not part of
the source):

- **Build Mac App.command** → `releases/PowerPC-Display-Mac.zip` (Universal;
  0.11 and 0.12 called it `-M1.zip`. The updater takes either, and on an
  Intel Mac only `-Mac.zip`)
- **Build PowerPC Display.command** on Tiger → `PowerPC-Display-PPC.zip`,
  sent automatically to `releases/` on the Apple Silicon Mac when
  **Share with PowerPC Mac.command** is running there. Builds from the UTM
  disc know where to send it (the disc lists the Mac's addresses); builds
  via `update.sh` remember the address that worked.

The Tiger zip contains the all-chips app (G3, G4, G5). The Mac app is signed
with a local certificate, so on other Macs it opens via Privacy & Security ›
Open Anyway.

## Tuning

- `-v` on both sides prints stats every 5 s. Apple Silicon: frames/s, KB/s,
  frame-to-screen latency, scroll copies, cache hits, video rects. PowerPC
  (`ppcdisplay -v`): frames/s, KB/s in, decode time, how long it waited for
  the graphics card, upload time and size per draw, and flush time (which
  includes waiting for the display's refresh). Send these when something
  feels slow: they show whether the CPU, the graphics card or the network is
  the limit.
- The PowerPC app does the same without Terminal: **Performance › Show
  Performance Stats**. The latest numbers appear at the bottom of its window
  (the session's average once you press Command-Escape or disconnect), and
  every 5-second line goes to `~/Library/Logs/PowerPC Display.log`
  (**Performance › Open Performance Log**). Each line ends with whether
  AltiVec and the hardware cursor were on, and both can be switched in the
  same menu to measure what they do on your Mac (AltiVec at once, the cursor
  from the next connection). Launching
  `"/Applications/PowerPC Display.app/Contents/MacOS/PowerPCDisplay" -v`
  from Terminal prints the stats there instead.
- `--inflight 1` on the Apple Silicon Mac trades a little smoothness for the lowest possible
  latency; `3` can help if the PowerPC Mac decodes faster than the network.
- `--fps 30` halves capture work; the PowerPC Mac still draws the cursor at full rate.
- `-c N` on the PowerPC Mac overrides the tile cache size (normally picked
  from RAM; each tile is 4 KB at 32-bit, 2 KB at 16-bit; `-c 0` turns it off).

## Design notes from research

- Apple's texture guidance: client storage + texture range + rectangle
  textures + `GL_BGRA`/`GL_UNSIGNED_INT_8_8_8_8_REV` is the zero-copy DMA path;
  rows must be a multiple of 32 bytes (1440×4 and 1440×2 both are). The
  *CACHED* storage hint is for textures drawn repeatedly, which ours is (every
  cursor move), so the client uses CACHED rather than SHARED.
- RDP and xrdp cut bandwidth with client-side bitmap caches; g4display does
  the same with 32×32 tiles. The Apple Silicon Mac keeps an exact mirror, so a hash
  collision can't show wrong pixels. Tiles changing at video rate (<100 ms
  apart) aren't cached, so video doesn't flush it.
- LZ4 decoding uses fixed 8-byte copies for short literals/matches (the
  reference decoder's trick), avoiding a `memcpy` call per sequence. Large
  copies go through Tiger's `bcopy`, which Apple tunes per chip (AltiVec on
  G4/G5).
- Direct framebuffer drawing copies with 8-byte floating-point stores rather
  than `memcpy`: the system copy uses `dcbz`, which traps on every cache line
  in uncached video memory.
- The pointer on the PowerPC Mac is its real (hardware) cursor, moved and
  re-shaped from the Apple Silicon Mac: moving the mouse costs the PowerPC
  Mac nothing and never delays a frame. If it misbehaves on some machine,
  go back to the drawn pointer with **Performance › Use Hardware Cursor**
  (or `defaults write local.ppcdisplay.client hardwareCursor -bool NO`).
  G3-era graphics (Rage 128) flicker the hardware cursor over a full-screen
  picture, so there the pointer is always drawn into the picture.
- G3s with a Rage 128 (no rectangle textures) draw straight into the
  screen's memory, with the screen switched to the picture's colour depth
  for the session: Tiger's Rage 128 OpenGL driver copies pictures in at
  about 20 MB/s, several times slower. There's no wait for the screen
  refresh, so fast scrolling can show a rare tear line;
  **Performance › Always Draw with OpenGL** goes back to OpenGL (which
  works on those GPUs too, with a power-of-two texture).
- Moves in any direction (a window being dragged, sideways scrolling) are
  found by sampling 16-pixel segments of what changed and finding them in
  the previous frame with a rolling hash; the largest block that moved
  exactly is one COPY.
- Text and interface areas (tiles with at most 256 colours, 16 at 16-bit)
  are grouped and sent as a palette plus 1-, 2-, 4- or 8-bit indices, then
  LZ4: typically 2–3 times less data than LZ4 on the pixels.
- Adaptive pacing: normally two frames are in flight, so the network and
  the PowerPC Mac's decoding overlap; while you move the mouse or scroll and
  frames take over 60 ms to reach the screen, only one, so the picture stays
  at most a frame behind your hand.
- Scrolling is found by matching rows in 32-pixel column bands, so a
  wallpaper edge, a sidebar, a sticky header or an overlay scrollbar next
  to the scrolling content doesn't hide it; the page moves with one COPY
  and only the newly revealed strip is sent.
- On G4 and G5 the video (4:2:2) conversion uses AltiVec, 8 pixels at a
  time; tested bit-identical to the plain version over every input
  combination.
- Only the 16-row bands that changed are uploaded to the graphics card, so
  a change at the top and one at the bottom of the screen don't upload
  everything in between.
- **Match video quality to the network** (on by default, with Smoother
  Video): while video plays, the Apple Silicon Mac sends it as sharp as the
  link allows. It starts with 4:2:2; when frames back up (the network or the
  PowerPC Mac can't keep up) it drops to half width and height (a quarter of
  the data, which the PowerPC Mac doubles back up, averaging in-between
  pixels), and after a calm while it tries a step better, up to lossless,
  if the measured link rate leaves room. A step up that didn't fit makes it
  wait longer before trying again. Only areas with the many colours of a
  picture that keep changing (not a scroll or a moving window) count as
  video, so text never gets softer. The current level shows under the stats
  ("sharp", "smooth", "half").
- Video regions use 4:2:2 YCbCr (Apple lists YCbCr 4:2:2 among the optimal
  texture formats). It's converted to RGB on the PowerPC CPU with integer
  maths identical on both Macs, so the Apple Silicon Mac's model of the
  screen stays exact (scroll detection and the tile cache keep working), and
  it works with every graphics card and drawing path.
- Tiger's TCP delayed-ACK default is a known cause of stalls on 10.4
  (HELIOS TI 110 recommends 2); see Fix Network Stalls above.

## Layout

```
host/Sources/CCodec/      protocol + codec, shared by both machines (C99)
host/Sources/CVirtualDisplay/  Obj-C wrapper for macOS virtual displays
host/Sources/CDiscovery/  Bonjour browse/resolve (dns_sd)
host/Sources/G4DisplayHost/    Apple Silicon app:
    Host.swift            state, settings, connect → pair → stream lifecycle
    HostDiscovery.swift   finding the PowerPC Mac (last address, Bonjour)
    Pairing.swift         HELLO, pairing, keychain, per-connection proof
    HostCapture.swift     virtual display, ScreenCaptureKit, changed areas → encoder
    HostSender.swift      encoding, flow control, pings, ACKs, cursor messages
    AppController, StatusWindow, MenuBar  window and menu bar UI;  Cursor.swift  pointer tracking
host/Sources/CCodec/g4auth.c   pairing crypto (X25519 + HMAC-SHA512), shared by both Macs
host/Sources/CCodec/g4nacl.c   TweetNaCl (public domain), vendored; RFC test vectors in test/
host/build-app.sh         builds PowerPC Display.app (Apple Silicon side)
g4/client.c               PowerPC side: CGL full screen, texture fast path, framebuffer fallback
g4/app.m                  PowerPC side: the Tiger app window
icon/make_icon.py         draws the app icon (modern and Tiger .icns)
test/                     round-trip + fuzz tests (`make -C test`)
```

## Status / honest caveats

- The video (4:2:2) path is tested the same way: the PowerPC side always
  equals the Apple Silicon Mac's model, including a little-endian encoder
  checked against a big-endian decoder, and becomes exact once video stops.
- Pairing is tested against RFC 7748 (X25519), SHA-512 and RFC 4231
  (HMAC-SHA512) vectors on x86 and emulated G3/G4, plus a simulated pairing,
  reconnect, impostor, decline and pulled-cable run of the real PowerPC engine
  against a fake Apple Silicon Mac.
- The codec is tested: every frame decodes byte-identical to what the Apple Silicon Mac
  captured, including scroll detection with and without hints, on x86 and on
  emulated big-endian PowerPC 7450, plus 9,000 corrupted frames under
  AddressSanitizer without a crash. The tile cache is covered by the same
  byte-exact checks.
- Both apps have run end to end (Apple Silicon Mac + Tiger in UTM). The G3
  and G5 tuning, the per-chip build and the new fallbacks have been
  compile-checked for each chip but not yet run on real G3/G4/G5 hardware.
- The virtual display uses macOS's private `CGVirtualDisplay` API (the same
  one BetterDisplay uses). If a future macOS breaks it, use `--display-id`
  with BetterDisplay.
- Video and fast animation will still be choppy: the PowerPC CPU is the limit.

# Notes for Claude: building, testing and shipping PowerPC Display

Read this before giving Colin build or release instructions. Every pitfall
below has actually happened. Give exact commands to copy (never placeholders
like `YOUR-MAC.local`), in the order below, and say what success looks like
at each step.

## The machines

- **Colin's Mac:** M1 iMac, "Colins-iMac-2" (Bonjour `Colins-iMac-2.local`),
  newest Xcode (27.x SDK as of Nov 2026). The project folder on it is
  `/Users/colinbalcarek/Downloads/g4display`. When this session is linked to
  that computer, it's mounted in the remote-devices shell as
  `~/mnt/g4display`.
- **The Tiger build machine:** a Mac OS X 10.4 VM in UTM on the M1 (QEMU; it
  reports itself as "Power Mac G4 AGP"). It builds the PowerPC app. Real
  PowerPC Macs: iMac G4 17" ("admin's iMac", 1440x900) and a G3 iBook
  ("Colin Balcarek's iBook", 1024x768, Rage 128).
- **GitHub:** repo `cbalcarek/powerpc-display` (the in-app update check needs it public). Commits go straight to
  `main`, as they have throughout.

## Getting changes onto Colin's Mac

Edit in the cloud copy of the repo, then copy each changed file to his Mac:
stage it under `/mnt/user-data/outputs/...` and run `device_commit_files`.
Copied files lose their execute bit, so afterwards run `chmod 755` on any
`*.command` and `*.sh` you sent, in the remote-devices shell. Check that the
copies match by comparing md5 sums of the source files on both sides.

## Build order: PowerPC app first, then the Mac app

The Mac app bundles `releases/PowerPC-Display-PPC.zip`, so the PowerPC zip
has to reach the Mac before the Mac app is built.

### The easy way (since 2026-11-03): one double-click on each Mac

1. On the M1: double-click **Rebuild Both Apps.command**. It shares the
   folder on port 8000 and waits.
2. In Tiger: double-click **Get Latest and Build.command** in
   `~/g4display/g4`. It runs `update.sh`, which downloads the latest source
   from the M1, builds, and sends the zip back.
   - Before that file exists in Tiger (anything older than this change), run
     once in Tiger's Terminal: `sh ~/g4display/g4/update.sh 10.0.2.2`
3. The M1 window notices the zip, stops sharing, and runs Build Mac App by
   itself (installs and opens the app).

No disc needed. **The Tiger VM reaches the M1 at `10.0.2.2`** (UTM emulated
network; confirmed 2026-11-03; `192.168.64.1` doesn't work for this VM).
`update.sh` remembers the address that worked in `g4/.m1host`.

### The disc way (first setup, or if the network route breaks)

1. On the M1: double-click **Share with PowerPC Mac.command** and leave it
   open (it serves the folder on port 8000 and accepts the built zip).
2. On the M1: double-click **Make ISO for UTM.command**. It writes
   `~/Downloads/g4display.iso`.
   - **Eject the old g4display disc in Tiger first.** If the ISO is replaced
     while Tiger has it mounted, Tiger reads garbage. The symptom: the build
     script fails on line 1 with nonsense like `ists: command not found`
     and README text in the error.
   - Then in UTM, choose the ISO again for the CD drive (restart the VM if
     the disc doesn't appear).
3. In Tiger: open the disc and double-click
   `g4display/g4/Build PowerPC Display.command`. It copies itself off the
   read-only CD to `~/g4display` and builds five steps (pairing code, then
   ppc750, 7400, 7450, 970).
   - Success looks like this:
     - `One app for G3, G4 and G5: ppc750 ppc7400 ppc7450 ppc970`
     - `Sent PowerPC-Display-PPC.zip to your Mac at …`
     - the Share window prints `received releases/PowerPC-Display-PPC.zip`.
4. If the zip didn't arrive, in Tiger run:
   `curl -T ~/g4display/g4/PowerPC-Display-PPC.zip http://10.0.2.2:8000/releases/PowerPC-Display-PPC.zip`
   - On 2026-11-02 the automatic send failed but this command worked. The
     likely cause was the old 60-second limit on the upload (the VM's
     emulated network is slow), now raised to 15 minutes.
   - "couldn't connect" while Share is running points at the Mac's firewall
     blocking `python3`.
5. `update.sh` must run from `~/g4display/g4`, never from the read-only CD.

### 2. Mac app (on the M1)

Double-click **Build Mac App.command**. It builds arm64 and x86_64
separately (`host/build-app.sh`, each in `.build/<arch>` with an explicit
`<arch>-apple-macosx12.3` triple), joins them with lipo, signs with the
local certificate, installs to /Applications, and writes
`releases/PowerPC-Display-Mac.zip`.

- Success looks like:
  - `Built for: x86_64 arm64`
  - `Runs on macOS 12.3 and later.` (lowered from 14 on 2026-11-14 for
    Colin's 2013 Mac Pro on Monterey: sound needs 13, muting it on the Mac
    needs 14.2, Launch at login needs 13; all checked with `#available`)
- **After any Xcode update** the first build stops at the licence:
  `Agreeing to the Xcode and Apple SDKs license requires admin privileges`.
  Fix: `sudo xcodebuild -license accept` in Terminal, then build again.
- Newer Xcodes put every architecture in `.build/out`, which is why each
  build has its own scratch path; otherwise lipo fails with "same
  architectures".
- Harmless warnings: `g4nacl.c … loses integer precision`,
  `AppUpdater … Sendable`, and `x86_64 architecture is deprecated`.

### 3. The PowerPC Macs

They update from the M1: open PowerPC Display on the G4 or G3, let the M1
connect, and choose **Install Update…**. If it's greyed out, Disconnect and
then Connect from the M1 (the offer is made on connect, and rechecked every
30 s).

## Releases

- Attach exactly `PowerPC-Display-Mac.zip` (Universal; `-M1.zip` in 0.11
  and 0.12) and `PowerPC-Display-PPC.zip`.
- Never attach both `-M1` and `-Mac`: 0.12's updater would prefer `-M1`.
- Versions are set in `host/build-app.sh` and
  `g4/Build PowerPC Display.command` (CFBundleShortVersionString and
  CFBundleVersion). 0.11 shipped; check GitHub for 0.12; 0.13 is built,
  not released. Bump both files for the next release.
- Give release notes plus a prefilled
  `https://github.com/cbalcarek/powerpc-display/releases/new?tag=…&title=…&body=…`
  link. Suggest **pre-release** until he has tested on hardware: installed
  copies offer any published release to themselves.

## Checking work here (Linux cloud session)

- Swift can't be compiled here. Review it carefully, because Colin's build
  is the first compile. Watch for things newer Xcode flags: NSLock in async
  functions, and Sendable captures.
- `test/test_codec.c`: build with
  `gcc -O2 -I host/Sources/CCodec/include test/test_codec.c host/Sources/CCodec/g4codec.c host/Sources/CCodec/g4yuv_altivec.c -lm`.
  `./a.out audio` runs just the sound test; the full suite takes a few
  minutes.
- `sh test/sim/run.sh` runs the PowerPC client against fake Tiger APIs and a
  fake Mac: keyboard/mouse signing, sound playback, and the picture stream.
- Big-endian check: `powerpc-linux-gnu-gcc -static -mcpu=750` and `qemu-ppc -cpu 750`
  (both installed) run codec code as a G3 would.
- Tiger's gcc 4.0 limits: C89-style declarations at the top of blocks, no
  `__sync` builtins (use `OSMemoryBarrier`), and it can run out of memory on
  big AltiVec files (that's why AltiVec lives in `g4yuv_altivec.c`).

## Troubleshooting a live session

Ask Colin to run this on the M1 after reproducing the problem. It saves the
Mac app's log into the project folder, where it can be read through the
device bridge (`~/mnt/g4display/m1-log.txt`):

```
log show --predicate 'subsystem == "local.ppcdisplay.host"' --last 15m > ~/Downloads/g4display/m1-log.txt
```

While it's sending, the log has:
- the Mac's stats every ~2 s (fps, KB/s, frame→screen, scroll copies…)
- the PowerPC Mac's own stats every 5 s (`"admin's iMac" says: … decode …
  GPU wait … upload …`, PowerPC app 0.13+)
- `first click from …` when PowerPC input clicks arrive

The PowerPC Mac's log (Performance › Open Log) also says how it saw the
first click: `first click: polling sees …, app events see …`.

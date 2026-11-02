# Two desks on one Mac: a PowerPC Mac on a virtual Mac

Research notes and a setup guide for running two independent "desks" on one
Apple Silicon Mac (your M1 iMac now, a Mac mini later):

- **Desk 1:** the Mac's own desktop, shown on the first PowerPC Mac.
- **Desk 2:** a macOS virtual machine in UTM, with PowerPC Display running
  inside it, shown on the second PowerPC Mac.

Each PowerPC Mac uses its own keyboard and mouse, and can play its desk's
sound (PowerPC Display 0.13). This is research plus a plan. Nothing here has
been tried on real hardware yet. "Step 3" at the end lists the app changes
this will most likely need, to build once the first test shows what's
missing.

## Limits worth knowing first

- **Two macOS virtual machines at once, per Mac.** Apple's licence (still so
  in macOS 27) allows two extra copies of macOS in virtual machines on each
  Mac you own, and macOS enforces it. Every app shares the same two slots:
  UTM, Parallels, VMware. So one Mac gives at most three desks: its own
  plus two virtual Macs. Desk 2 needs just one.
- **Memory is the real limit on an M1.** Plan about 6 GB for the virtual
  Mac on a 16 GB M1, and about 4 GB on an 8 GB M1 (tight: both desks will
  feel slow with a browser open). The PowerPC Macs themselves need little.
- **Use macOS 15 Sequoia inside the virtual Mac, not macOS 26 Tahoe.**
  In Tahoe virtual machines, app windows (and video) don't show up in screen
  capture: only the wallpaper, menu bar and Dock do. Two separate reports
  describe it, with Apple feedback filed (FB21748086) and no fix as of
  macOS 26.5.1. Sequoia guests are reported to work. PowerPC Display needs
  macOS 14 or later, so Sequoia is fine. The Mac itself can stay on Tahoe.

## What you need

- UTM (free, from mac.getutm.app or the App Store), on the Apple Silicon Mac.
- A macOS 15 Sequoia restore image (`.ipsw`). UTM downloads the newest
  compatible macOS by itself, which would be Tahoe, so get a Sequoia IPSW
  instead. Apple hosts them; ipsw.me lists Apple's own download links.
- About 60 GB free disk space for the virtual Mac.
- The second PowerPC Mac on the same network as the Mac, or on the same
  cable or switch.

## Setting it up

1. **Make the virtual Mac.** In UTM: **Create a New Virtual Machine ›
   Virtualize › macOS 12+**, and choose the Sequoia IPSW. Give it 4 CPU cores
   and the memory above, and a disk of 60 GB or more.
2. **Network: Bridged, not Shared.** In the virtual Mac's settings, set the
   network to **Bridged (Advanced)** and pick the interface the PowerPC Mac
   is on (Ethernet if it's cabled). With UTM's default **Shared Network**,
   the virtual Mac sits behind a NAT of its own:
   - Bonjour can't see the PowerPC Mac, so it's never found automatically.
   - Wake-on-LAN can't reach it.

   UTM warns that bridging over Wi-Fi may need extra setup, so Ethernet is
   the easy path.
3. **Display:** set its resolution to the second PowerPC Mac's screen
   (1440 × 900 for the iMac G4 17", 1024 × 768 for the iBook), with HiDPI
   off. That makes UTM's own window a 1:1 preview of desk 2.
4. **Sound:** set it to **Output Only**, so the virtual Mac has a sound
   device. To be tested: whether desk 2's sound then also plays from the
   Mac's own speakers through UTM. If it does, lower the virtual Mac's
   volume in UTM.
5. **Inside the virtual Mac:**
   - Turn on automatic login: System Settings › Users & Groups. FileVault
     has to be off, which it is by default in a virtual Mac.
   - Install PowerPC Display. Allow **Screen Recording**, **Local
     Network** and **Accessibility**.
   - Turn on **Launch at login** and **Use as this Mac's main display**.
   - Pair it with the second PowerPC Mac.
   - Turn off display sleep in the virtual Mac (System Settings › Lock
     Screen), or the PowerPC Mac is told to sleep when it times out. Typing
     on the PowerPC Mac counts as activity, so this matters mostly when
     nobody's using it.
6. **On the Mac itself (desk 1):** pair with the first PowerPC Mac as
   usual.
7. **Keep each PowerPC Mac with its own desk.** Both the Mac and the
   virtual Mac will see both PowerPC Macs on the network. Use **Remove from
   List…** for the other desk's PowerPC Mac on each side, and click
   **Don't Pair** if one asks. That's a stopgap: see step 3, item 1.

## Things the first test has to answer

| Question | Why it matters | If it fails |
|---|---|---|
| Does PowerPC Display's own virtual display work inside the virtual Mac? | It's how headless mode adds the PowerPC Mac's screen. One report created one fine inside a Tahoe guest. | Use the virtual Mac's own display instead (step 3, item 2). |
| Does screen capture keep up inside the virtual Mac? | Another project found that in Apple's virtual machines, some screen updates don't produce capture frames, and added a fallback poll. | Add a fallback capture when the screen seems idle (step 3, item 3). |
| Does the virtual Mac's sound reach the PowerPC Mac? | Needs a sound device in the guest, and capture of it. | Try the other UTM sound modes. |
| Do keyboard and mouse work? | Accessibility works the same in a guest, so this should be fine. | — |
| Picture speed on desk 2 | The virtual Mac's graphics are slower than the real Mac's. | Lower the frame rate for that desk. |
| Sleep | When the Mac sleeps, the virtual Mac pauses. PowerPC Mac 2 should fall back to its own desktop and reconnect on wake. | Note what happens. |

## Step 3: app changes this will likely need (not built yet)

1. **"Don't use this PowerPC Mac on this Mac."** Today **Remove from List**
   only lasts until the PowerPC Mac shows up on the network again. Two desks
   on one network need a lasting choice of which PowerPC Mac belongs to
   which desk, or each will keep asking to pair with the other's.
2. **Use the Mac's own display for a PowerPC Mac.** The engine can already
   capture an existing display instead of adding one (the `--display-id`
   option). The virtual Mac's own display, set to the PowerPC Mac's
   resolution, may be more reliable than a virtual display inside a virtual
   machine. This needs a setting in the window.
3. **A fallback capture when the screen seems idle**, if screen capture in
   the virtual Mac misses updates.
4. **Connect to a PowerPC Mac by address**, in the window. This is only
   needed if bridged networking isn't possible: a virtual Mac behind UTM's
   shared network can still reach the PowerPC Mac directly, just not find
   it. Today the address can only be given on the command line.
5. **A setup checklist in the window** for a virtual Mac: the permissions,
   automatic login and display sleep from above.

## Sources

- Apple's macOS 27 licence and the two-VM limit: [Apple Developer Forums](https://developer.apple.com/forums/thread/845455), [Jürgen Koller](https://juergenkoller.software/en/blog/more-than-two-macos-vms-on-apple-silicon.html)
- Tahoe guests not showing windows or video in capture: [MacStadium known issues](https://docs.macstadium.com/remote-desktop-vdi/reference/known-issues), [cua #870](https://github.com/trycua/cua/issues/870), [cua #912](https://github.com/trycua/cua/issues/912)
- Capture frames missing in Apple virtual machines: [Sunshine PR #5107](https://github.com/LizardByte/Sunshine/pull/5107)
- UTM macOS guests, network, display and sound settings: [macOS guests](https://docs.getutm.app/guest-support/macos), [network](https://docs.getutm.app/settings-apple/devices/network/), [display](https://docs.getutm.app/settings-apple/devices/display/), [virtualization settings](https://docs.getutm.app/settings-apple/virtualization/)

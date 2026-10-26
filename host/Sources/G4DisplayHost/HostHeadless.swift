import CoreGraphics
import Foundation

// Headless: this Mac has no monitor (a Mac mini, say), so a PowerPC Mac is
// its screen. Its display is created at launch at the size it had last time,
// so it's there before the PowerPC Mac connects and stays while it's away;
// it becomes the main display (menu bar, Dock); and Mirror is off, since
// there's nothing to mirror.

extension Host {
    private var geometryKey: String? { cfg.g4Name.map { "displayGeometry.\($0)" } }

    var isHeadless: Bool { lock.lock(); defer { lock.unlock() }; return _headless }

    func setHeadless(_ on: Bool) {
        lock.lock()
        let changed = _headless != on, mirrorChosen = _mirror
        _headless = on
        lock.unlock()
        // reconnect extending instead of mirroring (or mirroring again)
        if changed, mirrorChosen { dropConnection() }
    }

    /// Connection thread, on connect: remember its screen for next launch.
    func rememberGeometry() {
        guard let k = geometryKey else { return }
        UserDefaults.standard.set([W, H, depth, widthMM], forKey: k)
    }

    /// Connection thread, while not connected: headless and no display yet,
    /// so create it now at the size it had last time.
    func prepareHeadlessDisplay() {
        guard isHeadless, isEnabled, cfg.fixedDisplayID == nil, vdisplay == nil,
              let k = geometryKey, let g = UserDefaults.standard.array(forKey: k) as? [Int],
              g.count == 4, g[0] > 0, g[1] > 0 else { return }
        (W, H, depth, widthMM) = (g[0], g[1], g[2], g[3])
        if peerName.isEmpty { peerName = cfg.g4Name ?? "" }
        if setUpDisplay() { log("headless: display for “\(displayLabel)” ready before it connects (\(W)x\(H))") }
    }

    /// Unattended pairing (nobody can see this Mac's screen) is allowed only
    /// while nothing is paired yet, or soon after PowerPC Display opens.
    static var launchedAt = nowMs()
    static let unattendedPairingWindowMs = 10 * 60_000.0
    /// Main thread: can anybody see an alert on this Mac? Set by HostGroup.
    static var screenVisible: () -> Bool = { true }
}

enum DisplayLayout {
    static func online() -> [CGDirectDisplayID] {
        var ids = [CGDirectDisplayID](repeating: 0, count: 32)
        var n: UInt32 = 0
        guard CGGetOnlineDisplayList(32, &ids, &n) == .success else { return [] }
        return Array(ids.prefix(Int(n)))
    }

    /// No monitor besides our own virtual displays.
    static func noMonitor(excluding ours: Set<CGDirectDisplayID>) -> Bool {
        !online().contains { !ours.contains($0) }
    }

    /// Make a display the main one by moving the arrangement so it's at the
    /// origin. false: not online yet, or macOS refused.
    static func makeMain(_ id: CGDirectDisplayID) -> Bool {
        let all = online()
        guard all.contains(id) else { return false }
        if CGMainDisplayID() == id { return true }
        let target = CGDisplayBounds(id).origin
        var c: CGDisplayConfigRef?
        guard CGBeginDisplayConfiguration(&c) == .success, let c else { return false }
        for d in all where CGDisplayMirrorsDisplay(d) == kCGNullDirectDisplay {
            let o = CGDisplayBounds(d).origin
            CGConfigureDisplayOrigin(c, d, Int32(o.x - target.x), Int32(o.y - target.y))
        }
        return CGCompleteDisplayConfiguration(c, .permanently) == .success
    }
}

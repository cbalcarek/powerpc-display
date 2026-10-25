import CDiscovery
import Foundation

/// One Host (connection, virtual display, capture) per PowerPC Mac, so
/// several can be extra displays at once. PowerPC Macs are found with
/// Bonjour and remembered, so a known one reconnects at its last address even
/// if Bonjour is stuck. Settings apply to all of them. Main thread unless
/// noted.
final class HostGroup {
    private static let knownKey = "knownPowerPCMacs"

    let base: Host.Config
    private(set) var hosts: [Host] = []
    /// a Host was added (main thread): connect its callbacks, then it starts
    var onAdd: ((Host) -> Void)?
    var onRemove: ((Host) -> Void)?

    // settings (the same for every PowerPC Mac)
    private(set) var fps: Int
    private(set) var isMirror: Bool
    private(set) var smoothVideo: Bool
    private(set) var fastVideo: Bool
    private var enabled = true

    init(config: Host.Config) {
        base = config
        fps = config.fps
        isMirror = config.mirror
        smoothVideo = config.smoothVideo
        fastVideo = config.fastVideo
    }

    /// Fixed address / name / display from the command line: one Host as before.
    private var single: Bool { base.g4Address != nil || base.g4Name != nil || base.fixedDisplayID != nil }

    func start() {
        startBrightnessSync()
        startLidWatch()
        if single { add(name: base.g4Name); return }
        var known = UserDefaults.standard.stringArray(forKey: Self.knownKey) ?? []
        // 0.10 and earlier remembered one PowerPC Mac
        if known.isEmpty, let d = UserDefaults.standard.dictionary(forKey: Host.legacyEndpointKey),
           let n = d["name"] as? String, !n.isEmpty {
            known = [n]
            UserDefaults.standard.set(known, forKey: Self.knownKey)
        }
        for n in known { add(name: n) }
        let t = Thread { [weak self] in self?.browseLoop() }
        t.qualityOfService = .utility
        t.start()
    }

    /// Background thread: every few seconds, start serving PowerPC Macs that
    /// appeared on the network.
    private func browseLoop() {
        var buf = [CChar](repeating: 0, count: 8192)
        while true {
            let n = g4_browse_names(2000, &buf, Int32(buf.count))
            if n > 0 {
                let names = String(cString: buf).split(separator: "\n").map(String.init)
                DispatchQueue.main.async {
                    for name in names where !self.hosts.contains(where: { $0.macName == name }) {
                        self.add(name: name)
                    }
                }
            }
            sleep(3)
        }
    }

    private func add(name: String?) {
        var cfg = base
        cfg.g4Name = name
        cfg.fps = fps; cfg.mirror = savedMirror(name); cfg.smoothVideo = smoothVideo; cfg.fastVideo = fastVideo
        let h = Host(config: cfg)
        hosts.append(h)
        if let name, !single {
            var known = UserDefaults.standard.stringArray(forKey: Self.knownKey) ?? []
            if !known.contains(name) { known.append(name); UserDefaults.standard.set(known, forKey: Self.knownKey) }
            log("serving “\(name)”")
        }
        onAdd?(h)
        if !enabled { h.setEnabled(false) }
        h.start()
    }

    /// Stop serving one PowerPC Mac and forget it (it comes back if it shows
    /// up on the network again).
    func forget(_ h: Host) {
        h.stop()
        hosts.removeAll { $0 === h }
        if let name = h.macName {
            let known = (UserDefaults.standard.stringArray(forKey: Self.knownKey) ?? []).filter { $0 != name }
            UserDefaults.standard.set(known, forKey: Self.knownKey)
        }
        onRemove?(h)
    }

    // MARK: settings for one PowerPC Mac

    private static func mirrorKey(_ name: String) -> String { "mirror.\(name)" }

    /// Extend or mirror, chosen per PowerPC Mac (a Mac not chosen for yet
    /// gets the old all-Macs setting, or --mirror / --extend).
    private func savedMirror(_ name: String?) -> Bool {
        guard let name, !single,
              let v = UserDefaults.standard.object(forKey: Self.mirrorKey(name)) as? Bool else { return isMirror }
        return v
    }

    func setMirror(_ on: Bool, for h: Host) {
        h.setMirror(on)
        if let name = h.macName { UserDefaults.standard.set(on, forKey: Self.mirrorKey(name)) }
    }

    /// Disconnect (its display is removed) or connect one PowerPC Mac.
    func setEnabled(_ on: Bool, for h: Host) { h.setEnabled(on) }

    // MARK: settings for all

    func setEnabled(_ on: Bool) { enabled = on; hosts.forEach { $0.setEnabled(on) } }
    func setSmoothVideo(_ on: Bool) { smoothVideo = on; hosts.forEach { $0.setSmoothVideo(on) } }
    func setFastVideo(_ on: Bool) { fastVideo = on; hosts.forEach { $0.setFastVideo(on) } }
    func setFPS(_ f: Int) { fps = f; hosts.forEach { $0.setFPS(f) } }
    func forgetPairings() {
        PairingStore.forgetAll()
        hosts.forEach { $0.dropConnection() }
    }
    func macWillSleep(_ why: String) { hosts.forEach { $0.macWillSleep(why) } }

    /// Every half second: the PowerPC Macs follow this Mac's brightness.
    private var brightnessTimer: Timer?
    private(set) var matchBrightness = true
    func setMatchBrightness(_ on: Bool) {
        matchBrightness = on
        if !on { hosts.forEach { h in h.encQ.async { h.sendBrightness(0xFFFF) } } }   // their own again
    }
    // MARK: laptop lid

    private var lidTimer: Timer?
    private var lidWasClosed = false

    /// A laptop with its lid closed sees our virtual displays as external
    /// monitors and stays awake (clamshell mode) instead of sleeping. So treat
    /// closing the lid as sleep, unless a real external monitor is attached
    /// (then clamshell mode is what you want): tell the PowerPC Macs to sleep
    /// and remove their displays, and macOS sleeps as usual.
    func startLidWatch() {
        lidTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self, let closed = Lid.isClosed() else { return }
            if closed == self.lidWasClosed { return }
            self.lidWasClosed = closed
            if closed {
                let ours = Set(self.hosts.compactMap { $0.virtualDisplayID })
                if Lid.realExternalDisplay(excluding: ours) {
                    log("lid closed with an external monitor: keeping the PowerPC Macs on")
                    return
                }
                self.hosts.forEach { $0.macWillSleep("the lid was closed", releaseDisplay: true) }
            } else {
                self.hosts.forEach { $0.macDidWake("the lid was opened") }
            }
        }
    }

    func startBrightnessSync() {
        brightnessTimer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
            guard let self, self.matchBrightness, let b = Brightness.current() else { return }
            let level = Int((b * 1000).rounded())
            self.hosts.forEach { h in h.encQ.async { h.sendBrightness(level) } }
        }
    }
    func macDidWake(_ why: String) { hosts.forEach { $0.macDidWake(why) } }
}

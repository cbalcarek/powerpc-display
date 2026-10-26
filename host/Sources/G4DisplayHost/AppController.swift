import AppKit
import ServiceManagement
import SwiftUI

/// One PowerPC Mac's status, for the UI. Main thread.
final class PeerModel: ObservableObject, Identifiable {
    let host: Host
    @Published var status: Host.Status = .searching(nil)
    @Published var stats: Host.Stats?
    @Published var mirror: Bool
    @Published var lastConnected: Date?
    var id: ObjectIdentifier { ObjectIdentifier(host) }
    var name: String? { host.macName }

    private var lastConnectedKey: String? { name.map { "lastConnected.\($0)" } }

    init(host: Host) {
        self.host = host
        mirror = host.mirrorChosen
        if let k = lastConnectedKey { lastConnected = UserDefaults.standard.object(forKey: k) as? Date }
    }

    /// Note the time while connected and when it disconnects ("Last connected …").
    func touchLastConnected() {
        lastConnected = Date()
        if let k = lastConnectedKey { UserDefaults.standard.set(lastConnected, forKey: k) }
    }

    /// Its name as the list shows it.
    var displayName: String {
        if let name { return name }
        switch status {
        case .connected(let n, _, _, _), .pairing(let n, _, _): return n
        default: return "PowerPC Mac"
        }
    }

    /// iBook / PowerBook: a laptop; anything else: a desktop.
    var deviceSymbol: String {
        let n = displayName.lowercased()
        return n.contains("book") ? "laptopcomputer" : "desktopcomputer"
    }

    /// mirroring right now (never while headless)
    var showsMirror: Bool { mirror && !host.isHeadless }

    var isConnected: Bool { if case .connected = status { return true }; return false }
    var isPairing: Bool { if case .pairing = status { return true }; return false }
    /// shown under "My PowerPC Macs" (the rest under "Other PowerPC Macs")
    var isActive: Bool { isConnected || isPairing }
    var isPaused: Bool { if case .paused = status { return true }; return false }

    /// A word or two for the list.
    var shortStatus: String {
        switch status {
        case .connected: return "Connected"
        case .pairing: return "Pairing"
        case .searching(nil): return "Not found"
        case .searching: return "Connecting…"
        case .paused: return "Disconnected"
        case .needsScreenRecording: return "Needs permission"
        case .failed, .cantReach: return "Can't connect"
        }
    }

    /// The line under its name in the list.
    var subtitle: String {
        if case .connected(_, let w, let h, let d) = status {
            return "\(w) × \(h) · \(d)-bit colour" + (showsMirror ? " · mirroring" : "")
        }
        if let t = lastConnected { return "Last connected " + t.formatted(.relative(presentation: .named)) }
        return name == nil ? "Open PowerPC Display on the PowerPC Mac" : "Not connected yet"
    }

    /// What's going on, for the details below the list.
    var detail: String {
        switch status {
        case .searching(nil):
            if name == nil {
                return "Open PowerPC Display on the G3, G4 or G5. Both Macs need an Ethernet cable between them or the same network."
            }
            return host.canWake
                ? "Not on the network right now. It reconnects by itself when PowerPC Display opens on it. If it's asleep, click Wake."
                : "Not on the network right now. It reconnects by itself when PowerPC Display opens on it."
        case .searching(let addr?):
            return "Trying \(addr)… Open PowerPC Display on the PowerPC Mac if it isn't open."
        case .pairing(_, let code, let waiting):
            let c = String(format: "%03u %03u", code / 1000, code % 1000)
            return waiting
                ? "Now click Pair on the PowerPC Mac (it shows \(c))."
                : "Both Macs should show the code \(c). Pair only if they match."
        case .connected(_, let w, let h, _):
            return showsMirror ? "Mirroring your main display, scaled to \(w) × \(h)." : "Extending your desktop."
        case .paused:
            return "Its display is removed until you connect again."
        case .needsScreenRecording:
            return "Allow PowerPC Display in Privacy & Security › Screen & System Audio Recording, then quit and reopen it."
        case .failed(let why):
            return why + " It will retry automatically."
        case .cantReach(let why):
            return why + " Retrying automatically."
        }
    }

    /// stats are only meaningful if recent (nothing is sent while the screen is still)
    func liveStats(now: Double) -> Host.Stats? {
        guard let s = stats, now - s.at < 4000, s.fps > 0.5 else { return nil }
        return s
    }
}

/// Everything the UI shows. Updated on the main thread from Host callbacks.
final class AppModel: ObservableObject {
    @Published var peers: [PeerModel] = []
    @Published var selectedID: ObjectIdentifier?
    @Published var fps: Int
    @Published var smoothVideo = true
    @Published var fastVideo = true
    @Published var matchBrightness = true
    @Published var launchAtLogin = false
    /// "Use as this Mac's display" switch, and whether headless is in effect
    /// (also when there's no monitor)
    @Published var headlessChosen = false
    @Published var headless = false

    init(fps: Int) { self.fps = fps }

    var connected: [PeerModel] { peers.filter(\.isConnected) }
    var isConnected: Bool { !connected.isEmpty }

    /// the Mac whose details show: the one clicked, else a connected one
    var selected: PeerModel? {
        peers.first { $0.id == selectedID } ?? peers.first(where: \.isActive) ?? peers.first
    }

    /// one line for the menu bar icon's tooltip and the menu's first line
    var title: String {
        let c = connected
        if c.count == 1 { return "Connected to “\(c[0].displayName)”" }
        if c.count > 1 { return "Connected to \(c.count) PowerPC Macs" }
        return "No PowerPC Mac connected"
    }
}

final class AppController: NSObject, NSApplicationDelegate {
    let hosts: HostGroup
    let updater = AppUpdater()
    let model: AppModel
    private var menuBar: MenuBarController?
    private var window: NSWindow?

    static let fpsChoices = [60, 30, 20, 15]
    private static let fpsKey = "fps"
    private static let mirrorKey = "mirror"
    private static let smoothKey = "smoothVideo"
    private static let fastKey = "fastVideo"
    private static let brightKey = "matchBrightness"
    static var savedMirror: Bool { UserDefaults.standard.bool(forKey: mirrorKey) }
    static var savedSmoothVideo: Bool { UserDefaults.standard.object(forKey: smoothKey) as? Bool ?? true }
    static var savedFastVideo: Bool { UserDefaults.standard.object(forKey: fastKey) as? Bool ?? true }
    static var savedFPS: Int? {
        let v = UserDefaults.standard.integer(forKey: fpsKey)
        return v > 0 ? v : nil
    }

    var isBundled: Bool { Bundle.main.bundleIdentifier != nil }

    init(hosts: HostGroup) {
        self.hosts = hosts
        self.model = AppModel(fps: hosts.fps)
        super.init()
        model.smoothVideo = hosts.smoothVideo
        model.fastVideo = hosts.fastVideo
        let mb = UserDefaults.standard.object(forKey: Self.brightKey) as? Bool ?? true
        model.matchBrightness = mb
        hosts.setMatchBrightness(mb)
        model.headlessChosen = hosts.headlessChosen
        model.headless = hosts.isHeadless
        hosts.onHeadlessChange = { [weak self] in
            guard let self else { return }
            self.model.headless = self.hosts.isHeadless
            self.model.headlessChosen = self.hosts.headlessChosen
        }
    }

    // MARK: - lifecycle

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMainMenu()
        if isBundled { model.launchAtLogin = SMAppService.mainApp.status == .enabled }

        let mb = MenuBarController(controller: self)
        menuBar = mb
        hosts.onAdd = { [weak self] h in
            guard let self else { return }
            let peer = PeerModel(host: h)
            h.onStatus = { [weak self, weak peer] s in
                guard let peer else { return }
                var nowConnected = false
                if case .connected = s { nowConnected = true }
                if peer.isConnected || nowConnected { peer.touchLastConnected() }   // connecting or just disconnected
                peer.status = s
                if !peer.isConnected { peer.stats = nil }
                self?.model.objectWillChange.send()
                self?.menuBar?.updateIcon()
            }
            h.onStats = { [weak peer] s in peer?.stats = s }
            self.model.peers.append(peer)
            self.menuBar?.updateIcon()
        }
        hosts.onRemove = { [weak self] h in
            guard let self else { return }
            self.model.peers.removeAll { $0.host === h }
            if self.model.selectedID != nil, self.model.selected?.id != self.model.selectedID { self.model.selectedID = nil }
            self.menuBar?.updateIcon()
        }

        showWindow()
        if !CGPreflightScreenCaptureAccess() { _ = CGRequestScreenCaptureAccess() }
        hosts.start()

        // sleep and wake with this Mac (and its displays)
        let ws = NSWorkspace.shared.notificationCenter
        ws.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in
            self?.hosts.macWillSleep("this Mac is going to sleep")
        }
        ws.addObserver(forName: NSWorkspace.screensDidSleepNotification, object: nil, queue: .main) { [weak self] _ in
            self?.hosts.macWillSleep("the displays went to sleep")
        }
        ws.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.hosts.macDidWake("this Mac woke up")
        }
        ws.addObserver(forName: NSWorkspace.screensDidWakeNotification, object: nil, queue: .main) { [weak self] _ in
            self?.hosts.macDidWake("the displays woke up")
        }
        // quietly, at most once a day: a new release, and the PowerPC app for PowerPC Macs
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { [weak self] in self?.updater.check(userInitiated: false) }
        log("PowerPC Display running — status window open, menu bar icon added")
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        showWindow()       // clicking the Dock icon brings the window back
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    // MARK: - window

    @objc func showWindow() {
        if window == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 640),
                             styleMask: [.titled, .closable, .miniaturizable],
                             backing: .buffered, defer: false)
            w.title = "PowerPC Display"
            w.isReleasedWhenClosed = false
            let hc = NSHostingController(rootView: StatusView(model: model, controller: self))
            hc.sizingOptions = [.preferredContentSize]     // the window grows with the list of Macs
            w.contentViewController = hc
            w.center()
            w.setFrameAutosaveName("PowerPCDisplayWindow3")
            window = w
        }
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func buildMainMenu() {
        let main = NSMenu()

        let appItem = NSMenuItem()
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About PowerPC Display", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        let show = appMenu.addItem(withTitle: "Show Status Window", action: #selector(showWindow), keyEquivalent: "0")
        show.target = self
        let forget = appMenu.addItem(withTitle: "Forget Paired PowerPC Macs…", action: #selector(forgetPairingsAction), keyEquivalent: "")
        forget.target = self
        let upd = appMenu.addItem(withTitle: "Check for Updates…", action: #selector(checkForUpdates), keyEquivalent: "")
        upd.target = self
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide PowerPC Display", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(withTitle: "Quit PowerPC Display", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        main.addItem(appItem)

        let winItem = NSMenuItem()
        let winMenu = NSMenu(title: "Window")
        winMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        winMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        winItem.submenu = winMenu
        main.addItem(winItem)
        NSApp.windowsMenu = winMenu

        NSApp.mainMenu = main
    }

    @objc private func forgetPairingsAction() { forgetPairings() }
    @objc func checkForUpdates() { updater.check(userInitiated: true) }

    // MARK: - actions (used by both the window and the menu bar)

    /// Connect or disconnect one PowerPC Mac.
    func setConnected(_ on: Bool, _ peer: PeerModel) {
        if !on, model.headless, model.connected.count == 1, peer.isConnected {
            // it's the only screen: say how to get it back
            let a = NSAlert()
            a.messageText = "Disconnect “\(peer.displayName)”?"
            a.informativeText = "It's this Mac's only display. To connect again, plug in a monitor or use Screen Sharing from another Mac, or quit and reopen PowerPC Display."
            a.addButton(withTitle: "Disconnect")
            a.addButton(withTitle: "Cancel")
            guard a.runModal() == .alertFirstButtonReturn else { return }
        }
        hosts.setEnabled(on, for: peer.host)
    }

    func setHeadless(_ on: Bool) {
        hosts.setHeadless(on)
        model.headlessChosen = on
        model.headless = hosts.isHeadless
    }

    /// Extend or mirror, per PowerPC Mac.
    func setMirror(_ on: Bool, _ peer: PeerModel) {
        peer.mirror = on
        hosts.setMirror(on, for: peer.host)
    }

    func wake(_ peer: PeerModel) { peer.host.wakeNow() }

    /// Show one PowerPC Mac's details in the window.
    func select(_ peer: PeerModel) {
        model.selectedID = peer.id
        showWindow()
    }

    func setSmoothVideo(_ on: Bool) {
        model.smoothVideo = on
        hosts.setSmoothVideo(on)
        UserDefaults.standard.set(on, forKey: Self.smoothKey)
    }

    func setFastVideo(_ on: Bool) {
        model.fastVideo = on
        hosts.setFastVideo(on)
        UserDefaults.standard.set(on, forKey: Self.fastKey)
    }

    func setMatchBrightness(_ on: Bool) {
        model.matchBrightness = on
        hosts.setMatchBrightness(on)
        UserDefaults.standard.set(on, forKey: Self.brightKey)
    }

    func setFPS(_ f: Int) {
        model.fps = f
        hosts.setFPS(f)
        UserDefaults.standard.set(f, forKey: Self.fpsKey)
    }

    func setLaunchAtLogin(_ on: Bool) {
        guard isBundled else { return }
        do {
            if on { try SMAppService.mainApp.register() } else { try SMAppService.mainApp.unregister() }
        } catch {
            log("Launch at Login: \(error.localizedDescription)")
        }
        model.launchAtLogin = SMAppService.mainApp.status == .enabled
    }

    func forgetPairings() {
        let a = NSAlert()
        a.messageText = "Forget paired PowerPC Macs?"
        a.informativeText = "Each PowerPC Mac will have to pair again, showing a code on both Macs, the next time it connects."
        a.addButton(withTitle: "Forget")
        a.addButton(withTitle: "Cancel")
        if a.runModal() == .alertFirstButtonReturn { hosts.forgetPairings() }
    }

    /// Stop showing one PowerPC Mac that isn't connected (an old VM, say).
    func forget(_ peer: PeerModel) {
        let a = NSAlert()
        a.messageText = "Remove “\(peer.displayName)” from the list?"
        a.informativeText = "PowerPC Display stops looking for it. If it shows up on the network again, it comes back."
        a.addButton(withTitle: "Remove")
        a.addButton(withTitle: "Cancel")
        if a.runModal() == .alertFirstButtonReturn { hosts.forget(peer.host) }
    }

    func openDisplaySettings() { open("x-apple.systempreferences:com.apple.Displays-Settings.extension") }
    func openPrivacySettings() { open("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture") }

    private func open(_ url: String) {
        if let u = URL(string: url) { NSWorkspace.shared.open(u) }
    }
}


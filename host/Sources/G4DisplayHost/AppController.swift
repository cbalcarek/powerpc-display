import AppKit
import ServiceManagement
import SwiftUI

/// One PowerPC Mac's status, for the UI. Main thread.
final class PeerModel: ObservableObject, Identifiable {
    let host: Host
    @Published var status: Host.Status = .searching(nil)
    @Published var stats: Host.Stats?
    var id: ObjectIdentifier { ObjectIdentifier(host) }
    var name: String? { host.macName }

    init(host: Host) { self.host = host }

    var title: String {
        switch status {
        case .searching(nil): return name.map { "Looking for “\($0)”…" } ?? "Looking for your PowerPC Mac…"
        case .searching(let addr?): return "Waiting for the PowerPC Mac at \(addr)…"
        case .pairing(let name, _, _): return "Pair with “\(name)”?"
        case .connected(let name, _, _, _): return "Connected to “\(name)”"
        case .paused: return name.map { "Disconnected from “\($0)”" } ?? "Disconnected"
        case .needsScreenRecording: return "Screen Recording permission needed"
        case .failed(let why): return why
        case .cantReach: return name.map { "Not connected to “\($0)”" } ?? "Not connected to the PowerPC Mac"
        }
    }

    func detail(mirror: Bool) -> String {
        switch status {
        case .searching(nil):
            return "Open PowerPC Display on the G3, G4 or G5. Both Macs need an Ethernet cable between them or the same network."
        case .searching:
            return "Open PowerPC Display on the PowerPC Mac."
        case .pairing(_, let code, let waiting):
            let c = String(format: "%03u %03u", code / 1000, code % 1000)
            return waiting
                ? "Now click Pair on the PowerPC Mac (it shows \(c))."
                : "Both Macs should show the code \(c). Pair only if they match."
        case .connected(_, let w, let h, let d):
            return mirror
                ? "Mirroring your main display, scaled to \(w) × \(h) · \(d)-bit colour"
                : "\(w) × \(h) · \(d)-bit colour · arrange it in System Settings › Displays"
        case .paused:
            return "The PowerPC Mac's display is removed until you connect again."
        case .needsScreenRecording:
            return "Allow PowerPC Display in Privacy & Security › Screen & System Audio Recording, then quit and reopen it."
        case .failed:
            return "It will retry automatically."
        case .cantReach(let why):
            return why + " Retrying automatically."
        }
    }

    var symbol: String {
        switch status {
        case .connected: return "display"
        case .pairing: return "lock"
        case .paused: return "pause.circle"
        case .needsScreenRecording, .failed, .cantReach: return "exclamationmark.triangle"
        case .searching: return "antenna.radiowaves.left.and.right"
        }
    }

    var isConnected: Bool { if case .connected = status { return true }; return false }

    /// stats are only meaningful if recent (nothing is sent while the screen is still)
    func liveStats(now: Double) -> Host.Stats? {
        guard let s = stats, now - s.at < 4000, s.fps > 0.5 else { return nil }
        return s
    }
}

/// Everything the UI shows. Updated on the main thread from Host callbacks.
final class AppModel: ObservableObject {
    @Published var peers: [PeerModel] = []
    @Published var enabled = true
    @Published var fps: Int
    @Published var mirror = false
    @Published var smoothVideo = true
    @Published var fastVideo = true
    @Published var launchAtLogin = false

    init(fps: Int) { self.fps = fps }

    var connected: [PeerModel] { peers.filter(\.isConnected) }
    var isConnected: Bool { !connected.isEmpty }

    /// one line for the menu bar icon's tooltip and the menu's first line
    var title: String {
        let c = connected
        if c.count == 1 { return c[0].title }
        if c.count > 1 { return "Connected to \(c.count) PowerPC Macs" }
        return peers.first?.title ?? "Looking for your PowerPC Mac…"
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
        model.mirror = hosts.isMirror
        model.smoothVideo = hosts.smoothVideo
        model.fastVideo = hosts.fastVideo
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
            self?.model.peers.removeAll { $0.host === h }
            self?.menuBar?.updateIcon()
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
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 480, height: 290),
                             styleMask: [.titled, .closable, .miniaturizable],
                             backing: .buffered, defer: false)
            w.title = "PowerPC Display"
            w.isReleasedWhenClosed = false
            let hc = NSHostingController(rootView: StatusView(model: model, controller: self))
            hc.sizingOptions = [.preferredContentSize]     // the window grows with the list of Macs
            w.contentViewController = hc
            w.center()
            w.setFrameAutosaveName("PowerPCDisplayStatusWindow")
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

    func setEnabled(_ on: Bool) {
        model.enabled = on
        hosts.setEnabled(on)
    }

    func setMirror(_ on: Bool) {
        model.mirror = on
        hosts.setMirror(on)
        UserDefaults.standard.set(on, forKey: Self.mirrorKey)
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
        a.messageText = "Stop looking for “\(peer.name ?? "this PowerPC Mac")”?"
        a.informativeText = "It's removed from this list. If it shows up on the network again, it comes back."
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

// MARK: - status window

struct StatusView: View {
    @ObservedObject var model: AppModel
    let controller: AppController

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if model.peers.isEmpty {
                PeerRow(peer: nil, mirror: model.mirror, controller: controller)
            }
            ForEach(model.peers) { peer in
                PeerRow(peer: peer, mirror: model.mirror, controller: controller)
            }

            Divider()

            HStack {
                Text("Mode")
                Picker("Mode", selection: Binding(get: { model.mirror }, set: { controller.setMirror($0) })) {
                    Text("Extend").tag(false)
                    Text("Mirror").tag(true)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 180)
                Spacer()
            }

            HStack(spacing: 10) {
                Button(model.enabled ? "Disconnect" : "Connect") { controller.setEnabled(!model.enabled) }
                Button("Arrange Displays…") { controller.openDisplaySettings() }
                    .disabled(model.mirror)
                Spacer()
                Picker("Frame rate", selection: Binding(get: { model.fps }, set: { controller.setFPS($0) })) {
                    ForEach(AppController.fpsChoices, id: \.self) { Text("\($0) fps").tag($0) }
                }
                .labelsHidden()
                .frame(width: 90)
            }

            Toggle("Smoother video (slightly softer colours in videos)",
                   isOn: Binding(get: { model.smoothVideo }, set: { controller.setSmoothVideo($0) }))
                .help("Areas playing video are sent with half the data. Text and still areas stay exact.")

            Toggle("Match video quality to the network",
                   isOn: Binding(get: { model.fastVideo }, set: { controller.setFastVideo($0) }))
                .disabled(!model.smoothVideo)
                .padding(.leading, 20)
                .help("Video is sent as sharp as the connection allows: lossless when there's room, softer colour, or half resolution when the network or the PowerPC Mac can't keep up. Off: always softer colour, full resolution.")

            if controller.isBundled {
                Toggle("Launch at Login", isOn: Binding(get: { model.launchAtLogin },
                                                        set: { controller.setLaunchAtLogin($0) }))
            }
        }
        .padding(20)
        .frame(width: 480)
    }
}

/// One PowerPC Mac in the status window (nil: none found yet).
struct PeerRow: View {
    let peer: PeerModel?
    let mirror: Bool
    let controller: AppController

    var body: some View {
        if let peer { Row(peer: peer, mirror: mirror, controller: controller) }
        else {
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .font(.system(size: 30)).foregroundStyle(Color.secondary).frame(width: 40)
                VStack(alignment: .leading, spacing: 4) {
                    Text("Looking for your PowerPC Mac…").font(.headline)
                    Text("Open PowerPC Display on the G3, G4 or G5. Both Macs need an Ethernet cable between them or the same network.")
                        .font(.callout).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                }
            }
        }
    }

    private struct Row: View {
        @ObservedObject var peer: PeerModel
        let mirror: Bool
        let controller: AppController

        var body: some View {
            VStack(alignment: .leading, spacing: 10) {
                HStack(alignment: .top, spacing: 14) {
                    Image(systemName: peer.symbol)
                        .font(.system(size: 30, weight: .regular))
                        .foregroundStyle(peer.isConnected ? Color.accentColor : Color.secondary)
                        .frame(width: 40)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(peer.title).font(.headline)
                        Text(peer.detail(mirror: mirror))
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    Spacer()
                    if !peer.isConnected, peer.name != nil {
                        Button("Remove") { controller.forget(peer) }
                            .controlSize(.small)
                            .help("Stop looking for this PowerPC Mac")
                    }
                }
                if peer.isConnected {
                    TimelineView(.periodic(from: .now, by: 1)) { _ in
                        if let s = peer.liveStats(now: nowMs()) {
                            HStack(spacing: 24) {
                                stat(String(format: "%.0f", s.fps), "fps")
                                stat(String(format: "%.0f", s.kbps), "KB/s")
                                stat(String(format: "%.0f", s.latencyMs), "ms to screen")
                                if let q = s.videoQuality { stat(q, "video") }
                            }
                            .padding(.leading, 54)
                        } else {
                            Text("Idle — nothing on its screen is changing")
                                .font(.callout).foregroundStyle(.secondary)
                                .padding(.leading, 54)
                        }
                    }
                }
                if case .needsScreenRecording = peer.status {
                    Button("Open Screen Recording Settings…") { controller.openPrivacySettings() }
                        .padding(.leading, 54)
                }
            }
        }

        private func stat(_ value: String, _ label: String) -> some View {
            VStack(alignment: .leading, spacing: 0) {
                Text(value).font(.system(size: 22, weight: .semibold, design: .rounded)).monospacedDigit()
                Text(label).font(.caption).foregroundStyle(.secondary)
            }
        }
    }
}

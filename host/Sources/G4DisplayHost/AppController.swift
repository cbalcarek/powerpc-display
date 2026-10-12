import AppKit
import ServiceManagement
import SwiftUI

/// Everything the UI shows. Updated on the main thread from Host callbacks.
final class AppModel: ObservableObject {
    @Published var status: Host.Status = .searching(nil)
    @Published var stats: Host.Stats?
    @Published var enabled = true
    @Published var fps: Int
    @Published var mirror = false
    @Published var smoothVideo = true
    @Published var launchAtLogin = false

    init(fps: Int) { self.fps = fps }

    var title: String {
        switch status {
        case .searching(nil): return "Looking for your PowerPC Mac…"
        case .searching(let addr?): return "Waiting for the PowerPC Mac at \(addr)…"
        case .connected(let name, _, _, _): return "Connected to “\(name)”"
        case .paused: return "Disconnected"
        case .needsScreenRecording: return "Screen Recording permission needed"
        case .failed(let why): return why
        case .cantReach: return "Found the PowerPC Mac, but can't connect"
        }
    }

    var detail: String {
        switch status {
        case .searching(nil):
            return "Open PowerPC Display on the G3, G4 or G5. Both Macs need an Ethernet cable between them or the same network."
        case .searching:
            return "Open PowerPC Display on the PowerPC Mac."
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

final class AppController: NSObject, NSApplicationDelegate {
    let host: Host
    let model: AppModel
    private var menuBar: MenuBarController?
    private var window: NSWindow?

    static let fpsChoices = [60, 30, 20, 15]
    private static let fpsKey = "fps"
    private static let mirrorKey = "mirror"
    private static let smoothKey = "smoothVideo"
    static var savedMirror: Bool { UserDefaults.standard.bool(forKey: mirrorKey) }
    static var savedSmoothVideo: Bool { UserDefaults.standard.object(forKey: smoothKey) as? Bool ?? true }
    static var savedFPS: Int? {
        let v = UserDefaults.standard.integer(forKey: fpsKey)
        return v > 0 ? v : nil
    }

    var isBundled: Bool { Bundle.main.bundleIdentifier != nil }

    init(host: Host) {
        self.host = host
        self.model = AppModel(fps: host.fps)
        super.init()
        model.mirror = host.isMirror
        model.smoothVideo = host.smoothVideo
    }

    // MARK: - lifecycle

    func applicationDidFinishLaunching(_ notification: Notification) {
        buildMainMenu()
        if isBundled { model.launchAtLogin = SMAppService.mainApp.status == .enabled }

        let mb = MenuBarController(controller: self)
        menuBar = mb
        host.onStatus = { [weak self] s in
            guard let self else { return }
            self.model.status = s
            if !self.model.isConnected { self.model.stats = nil }
            self.menuBar?.updateIcon()
        }
        host.onStats = { [weak self] s in self?.model.stats = s }

        showWindow()
        if !CGPreflightScreenCaptureAccess() { _ = CGRequestScreenCaptureAccess() }
        host.start()
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
            w.contentView = NSHostingView(rootView: StatusView(model: model, controller: self))
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

    // MARK: - actions (used by both the window and the menu bar)

    func setEnabled(_ on: Bool) {
        model.enabled = on
        host.setEnabled(on)
    }

    func setMirror(_ on: Bool) {
        model.mirror = on
        host.setMirror(on)
        UserDefaults.standard.set(on, forKey: Self.mirrorKey)
    }

    func setSmoothVideo(_ on: Bool) {
        model.smoothVideo = on
        host.setSmoothVideo(on)
        UserDefaults.standard.set(on, forKey: Self.smoothKey)
    }

    func setFPS(_ f: Int) {
        model.fps = f
        host.setFPS(f)
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
            HStack(alignment: .top, spacing: 14) {
                Image(systemName: model.symbol)
                    .font(.system(size: 30, weight: .regular))
                    .foregroundStyle(model.isConnected ? Color.accentColor : Color.secondary)
                    .frame(width: 40)
                VStack(alignment: .leading, spacing: 4) {
                    Text(model.title).font(.headline)
                    Text(model.detail)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }

            if model.isConnected {
                TimelineView(.periodic(from: .now, by: 1)) { _ in
                    if let s = model.liveStats(now: nowMs()) {
                        HStack(spacing: 24) {
                            stat(String(format: "%.0f", s.fps), "fps")
                            stat(String(format: "%.0f", s.kbps), "KB/s")
                            stat(String(format: "%.0f", s.latencyMs), "ms to screen")
                        }
                    } else {
                        Text("Idle — nothing on its screen is changing")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                }
            }

            if case .needsScreenRecording = model.status {
                Button("Open Screen Recording Settings…") { controller.openPrivacySettings() }
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

            if controller.isBundled {
                Toggle("Launch at Login", isOn: Binding(get: { model.launchAtLogin },
                                                        set: { controller.setLaunchAtLogin($0) }))
            }
        }
        .padding(20)
        .frame(width: 480)
    }

    private func stat(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text(value).font(.system(size: 22, weight: .semibold, design: .rounded)).monospacedDigit()
            Text(label).font(.caption).foregroundStyle(.secondary)
        }
    }
}

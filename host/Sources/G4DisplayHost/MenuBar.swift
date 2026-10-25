import AppKit

/// Menu bar item: same status and controls as the window, always at hand.
final class MenuBarController: NSObject, NSMenuDelegate {
    private unowned let controller: AppController
    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private var model: AppModel { controller.model }

    init(controller: AppController) {
        self.controller = controller
        super.init()
        item.isVisible = true
        item.autosaveName = "PowerPCDisplayStatusItem"
        let menu = NSMenu()
        menu.delegate = self
        item.menu = menu
        updateIcon()
    }

    func updateIcon() {
        guard let button = item.button else { return }
        let img = NSImage(systemSymbolName: model.isConnected ? "display" : "display.trianglebadge.exclamationmark",
                          accessibilityDescription: "PowerPC Display")
            ?? NSImage(systemSymbolName: "display", accessibilityDescription: "PowerPC Display")
        img?.isTemplate = true
        button.image = img
        button.toolTip = model.title
    }

    // rebuilt every time it opens, so it's always current. Like the Wi-Fi
    // menu: a line per PowerPC Mac, each with its own submenu.
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let n = model.connected.count
        menu.addItem(info(n == 0 ? "PowerPC Display — none connected" : "PowerPC Display — \(n) connected"))
        menu.addItem(.separator())

        if model.peers.isEmpty { menu.addItem(info("Looking for PowerPC Macs…")) }
        for peer in model.peers.sorted(by: { $0.isActive && !$1.isActive }) {
            menu.addItem(peerItem(peer))
        }

        menu.addItem(.separator())
        let sv = action("Smoother Video", #selector(toggleSmooth))
        sv.state = model.smoothVideo ? .on : .off
        sv.toolTip = "Areas playing video are sent with half the data (slightly softer colours)."
        menu.addItem(sv)
        // greyed out (no action) unless Smoother Video is on
        let fv = model.smoothVideo ? action("Match Video Quality to Network", #selector(toggleFast))
                                   : info("Match Video Quality to Network")
        fv.state = model.fastVideo ? .on : .off
        fv.indentationLevel = 1
        fv.toolTip = "Video is sent as sharp as the connection allows: lossless, softer colour, or half resolution."
        menu.addItem(fv)
        let br = action("Match Brightness", #selector(toggleBrightness))
        br.state = model.matchBrightness ? .on : .off
        menu.addItem(br)

        let fpsItem = NSMenuItem(title: "Frame Rate", action: nil, keyEquivalent: "")
        let fpsMenu = NSMenu()
        for f in AppController.fpsChoices {
            let it = action("\(f) fps", #selector(setFPS(_:)))
            it.tag = f
            it.state = model.fps == f ? .on : .off
            fpsMenu.addItem(it)
        }
        fpsItem.submenu = fpsMenu
        menu.addItem(fpsItem)

        menu.addItem(.separator())
        menu.addItem(action("Show Status Window…", #selector(showWindow)))
        menu.addItem(action("Check for Updates…", #selector(checkUpdates)))
        menu.addItem(.separator())
        menu.addItem(action("Quit PowerPC Display", #selector(quit), key: "q"))
    }

    /// One PowerPC Mac: its name and state, with a submenu of what you can do.
    private func peerItem(_ peer: PeerModel) -> NSMenuItem {
        var status = peer.shortStatus
        if peer.isConnected, let s = peer.liveStats(now: nowMs()) { status = String(format: "%.0f fps", s.fps) }
        let it = NSMenuItem(title: peer.displayName, action: nil, keyEquivalent: "")
        if #available(macOS 14.4, *) {
            it.subtitle = status
        } else {
            it.title = "\(peer.displayName) — \(status)"
        }
        it.image = NSImage(systemSymbolName: peer.deviceSymbol, accessibilityDescription: nil)
        it.state = peer.isConnected ? .on : .off

        let sub = NSMenu()
        sub.addItem(info(peer.subtitle))
        if peer.isConnected, let s = peer.liveStats(now: nowMs()) {
            sub.addItem(info(String(format: "%.0f fps · %.0f KB/s · %.0f ms to screen", s.fps, s.kbps, s.latencyMs)))
        }
        sub.addItem(.separator())
        if peer.isConnected {
            sub.addItem(peerAction("Disconnect", #selector(disconnect(_:)), peer))
        } else if peer.isPaused {
            sub.addItem(peerAction("Connect", #selector(connect(_:)), peer))
        }
        if !peer.isActive, peer.host.canWake, !peer.isPaused {
            sub.addItem(peerAction("Wake", #selector(wake(_:)), peer))
        }
        if !peer.isPairing {
            let ext = peerAction("Extend Display", #selector(setExtend(_:)), peer)
            ext.state = peer.mirror ? .off : .on
            sub.addItem(ext)
            let mir = peerAction("Mirror Main Display", #selector(setMirror(_:)), peer)
            mir.state = peer.mirror ? .on : .off
            sub.addItem(mir)
        }
        if peer.isConnected, !peer.mirror {
            sub.addItem(action("Arrange Displays…", #selector(openDisplays)))
        }
        sub.addItem(.separator())
        sub.addItem(peerAction("Show Details…", #selector(showDetails(_:)), peer))
        it.submenu = sub
        return it
    }

    private func peerAction(_ title: String, _ sel: Selector, _ peer: PeerModel) -> NSMenuItem {
        let it = action(title, sel)
        it.representedObject = peer
        return it
    }

    private func info(_ text: String) -> NSMenuItem {
        let it = NSMenuItem(title: text, action: nil, keyEquivalent: "")
        it.isEnabled = false
        return it
    }

    private func action(_ title: String, _ sel: Selector, key: String = "") -> NSMenuItem {
        let it = NSMenuItem(title: title, action: sel, keyEquivalent: key)
        it.target = self
        return it
    }

    private func peer(_ sender: NSMenuItem) -> PeerModel? { sender.representedObject as? PeerModel }

    @objc private func showWindow() { controller.showWindow() }
    @objc private func showDetails(_ sender: NSMenuItem) { if let p = peer(sender) { controller.select(p) } }
    @objc private func connect(_ sender: NSMenuItem) { if let p = peer(sender) { controller.setConnected(true, p) } }
    @objc private func disconnect(_ sender: NSMenuItem) { if let p = peer(sender) { controller.setConnected(false, p) } }
    @objc private func wake(_ sender: NSMenuItem) { if let p = peer(sender) { controller.wake(p) } }
    @objc private func setExtend(_ sender: NSMenuItem) { if let p = peer(sender) { controller.setMirror(false, p) } }
    @objc private func setMirror(_ sender: NSMenuItem) { if let p = peer(sender) { controller.setMirror(true, p) } }
    @objc private func setFPS(_ sender: NSMenuItem) { controller.setFPS(sender.tag) }
    @objc private func openDisplays() { controller.openDisplaySettings() }
    @objc private func toggleSmooth() { controller.setSmoothVideo(!model.smoothVideo) }
    @objc private func toggleFast() { controller.setFastVideo(!model.fastVideo) }
    @objc private func toggleBrightness() { controller.setMatchBrightness(!model.matchBrightness) }
    @objc private func checkUpdates() { controller.checkForUpdates() }
    @objc private func quit() { NSApp.terminate(nil) }
}

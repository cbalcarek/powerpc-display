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

    // rebuilt every time it opens, so it's always current
    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        if model.peers.isEmpty { menu.addItem(info("Looking for your PowerPC Mac…")) }
        for peer in model.peers {
            menu.addItem(info(peer.title))
            if case .connected(_, let w, let h, let d) = peer.status {
                menu.addItem(info("    \(w) × \(h) · \(d)-bit colour"))
                if let s = peer.liveStats(now: nowMs()) {
                    menu.addItem(info(String(format: "    %.0f fps · %.0f KB/s · %.0f ms to screen", s.fps, s.kbps, s.latencyMs)))
                } else {
                    menu.addItem(info("    Idle — nothing changing"))
                }
            }
        }

        menu.addItem(.separator())
        menu.addItem(action("Show Status Window", #selector(showWindow)))
        menu.addItem(action(model.enabled ? "Disconnect" : "Connect", #selector(toggleEnabled)))
        let ext = action("Extend Display", #selector(setExtend))
        ext.state = model.mirror ? .off : .on
        menu.addItem(ext)
        let mir = action("Mirror Main Display", #selector(setMirror))
        mir.state = model.mirror ? .on : .off
        menu.addItem(mir)
        menu.addItem(action("Arrange Displays…", #selector(openDisplays)))
        let sv = action("Smoother Video", #selector(toggleSmooth))
        sv.state = model.smoothVideo ? .on : .off
        sv.toolTip = "Areas playing video are sent with half the data (slightly softer colours)."
        menu.addItem(sv)
        // greyed out (no action) unless Smoother Video is on
        let fv = model.smoothVideo ? action("Faster Video (Half Resolution)", #selector(toggleFast))
                                   : info("Faster Video (Half Resolution)")
        fv.state = model.fastVideo ? .on : .off
        fv.indentationLevel = 1
        fv.toolTip = "Video is sent at half width and height: a quarter of the data. It sharpens as soon as it stops moving."
        menu.addItem(fv)

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
        menu.addItem(action("Forget Paired PowerPC Macs…", #selector(forgetPairings)))
        menu.addItem(action("Check for Updates…", #selector(checkUpdates)))
        menu.addItem(action("Quit PowerPC Display", #selector(quit), key: "q"))
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

    @objc private func showWindow() { controller.showWindow() }
    @objc private func toggleEnabled() { controller.setEnabled(!model.enabled) }
    @objc private func setFPS(_ sender: NSMenuItem) { controller.setFPS(sender.tag) }
    @objc private func openDisplays() { controller.openDisplaySettings() }
    @objc private func setExtend() { controller.setMirror(false) }
    @objc private func setMirror() { controller.setMirror(true) }
    @objc private func toggleSmooth() { controller.setSmoothVideo(!model.smoothVideo) }
    @objc private func toggleFast() { controller.setFastVideo(!model.fastVideo) }
    @objc private func forgetPairings() { controller.forgetPairings() }
    @objc private func checkUpdates() { controller.checkForUpdates() }
    @objc private func quit() { NSApp.terminate(nil) }
}

import AppKit
import CCodec
import CoreGraphics
import Foundation

// Keyboard and mouse from the PowerPC Mac. While its picture is on screen it
// sends what its keys, pointer, buttons and scroll wheel do (signed with a
// key from pairing, so nobody else on the network can type on this Mac), and
// this Mac replays them as if a keyboard and mouse were plugged in here.
// Needs Accessibility permission (macOS asks once).

extension Host {
    var useInput: Bool { lock.lock(); defer { lock.unlock() }; return _useInput }

    func setUseInput(_ on: Bool) {
        lock.lock(); _useInput = on; lock.unlock()
        encQ.async { self.refreshInputOn() }
    }

    /// Allowed to post keyboard and mouse events (Accessibility).
    static var canPostEvents: Bool { CGPreflightPostEventAccess() }

    /// encQ: ask the PowerPC Mac for its keyboard and mouse, or tell it to
    /// stop, when that changed (setting, permission granted, new connection).
    func refreshInputOn() {
        guard fd >= 0 else { return }
        let want = clientInput && useInput && Host.canPostEvents
        guard want != sentInputOn else { return }
        guard writeAll(fd, message(type: Int(G4_MSG_INPUT_ON), payload: [want ? 1 : 0])) else { return }
        sentInputOn = want
        log(want ? "using “\(peerName)”'s keyboard and mouse" : "not using “\(peerName)”'s keyboard and mouse")
        if !want { injector.releaseAll() }
    }

    /// Connection thread: one G4_MSG_INPUT payload.
    func handleInput(_ p: [UInt8], _ len: Int) {
        let body = len - Int(G4_INPUT_MAC_LEN)
        guard let key = inputKey, body >= 4, (body - 4) % Int(G4_INPUT_EVENT_LEN) == 0 else { return }
        var mac = [UInt8](repeating: 0, count: Int(G4_INPUT_MAC_LEN))
        p.withUnsafeBufferPointer { b in g4_input_mac(&mac, key, b.baseAddress, body) }
        let ok = p.withUnsafeBufferPointer { b in g4_equal(mac, b.baseAddress! + body, mac.count) == 1 }
        let seq = be32(p, 0)
        guard ok, seq > lastInputSeq else {
            log("ignored keyboard/mouse input that wasn't signed by “\(peerName)”")
            return
        }
        lastInputSeq = seq
        guard useInput else { return }
        var events: [InputInjector.Event] = []
        var off = 4
        while off < body {
            let k = Int(p[off]), a = Int(p[off + 1])
            let b = Int(Int16(bitPattern: UInt16(be16(p, off + 2)))), c = Int(Int16(bitPattern: UInt16(be16(p, off + 4))))
            switch k {
            case Int(G4_IN_MOUSE):
                if a != 0, !loggedFirstClick {
                    loggedFirstClick = true
                    log("first click from “\(peerName)” (buttons \(a)) at \(b),\(c); replaying it here")
                }
                events.append(.mouse(buttons: a, at: inputPoint(b, c)))
            case Int(G4_IN_KEY): events.append(.key(code: UInt16(a), down: p[off + 2] != 0))
            case Int(G4_IN_SCROLL): events.append(.scroll(dy: b, dx: c))
            default: break
            }
            off += Int(G4_INPUT_EVENT_LEN)
        }
        injector.apply(events)
        encQ.async { self.lastInput = nowMs() }      // counts as using it (smoother pacing)
    }

    /// A PowerPC pixel -> a point on this Mac's desktop (its display, or the
    /// main display it mirrors, scaled and letterboxed).
    private func inputPoint(_ x: Int, _ y: Int) -> CGPoint {
        let m = inputMap
        let b = CGDisplayBounds(m.display)
        let s = m.scale > 0 ? m.scale : 1
        let px = min(max((CGFloat(x) - m.offX) / s, 0), max(b.width - 1, 0))
        let py = min(max((CGFloat(y) - m.offY) / s, 0), max(b.height - 1, 0))
        return CGPoint(x: b.minX + px, y: b.minY + py)
    }
}

/// Replays one PowerPC Mac's keyboard and mouse here. Keeps track of what's
/// held so it can let go of everything when the connection ends, repeats
/// held keys like a real keyboard, and counts double-clicks.
final class InputInjector {
    enum Event {
        case mouse(buttons: Int, at: CGPoint)
        case key(code: UInt16, down: Bool)
        case scroll(dy: Int, dx: Int)                   // tenths of a line
    }

    private let q = DispatchQueue(label: "ppcdisplay.inject", qos: .userInteractive)
    private let src = CGEventSource(stateID: .hidSystemState)
    private var buttons = 0
    private var pos = CGPoint.zero
    private var flags: CGEventFlags = []
    private var capsLock = false
    private var held = Set<UInt16>()                    // keys down (not modifiers)
    private var heldModifiers = Set<UInt16>()
    private var repeatTimer: DispatchSourceTimer?
    private var clickCount = 1, lastDownAt = 0.0, lastDownPos = CGPoint.zero
    private var scrollY = 0, scrollX = 0
    private let repeatDelay: Double, repeatInterval: Double

    init() {
        repeatDelay = NSEvent.keyRepeatDelay
        repeatInterval = NSEvent.keyRepeatInterval
    }

    func apply(_ events: [Event]) { q.async { events.forEach(self.handle) } }

    /// Let go of every key and button (connection ended, or input turned off).
    func releaseAll() {
        q.async {
            self.stopRepeat()
            for k in self.held { self.postKey(k, down: false) }
            self.held.removeAll()
            let mods = self.heldModifiers
            self.heldModifiers.removeAll()           // first, so each pair's flag clears
            for k in mods { self.setModifier(k, down: false) }
            for bit in [1, 2, 4] where self.buttons & bit != 0 { self.postButton(bit, down: false) }
            self.buttons = 0
        }
    }

    private func handle(_ e: Event) {
        switch e {
        case .key(let code, let down):
            if Self.modifierFlag(code) != nil || code == 57 {
                if down { heldModifiers.insert(code) } else { heldModifiers.remove(code) }
                setModifier(code, down: down)
            } else if down {
                held.insert(code)
                postKey(code, down: true)
                startRepeat(code)
            } else {
                held.remove(code)
                if repeating == code { stopRepeat() }
                postKey(code, down: false)
            }
        case .mouse(let b, let p):
            let moved = p != pos
            let old = buttons
            pos = p
            if moved {
                let type: CGEventType = b & 1 != 0 && old & 1 != 0 ? .leftMouseDragged
                    : b & 2 != 0 && old & 2 != 0 ? .rightMouseDragged
                    : b & 4 != 0 && old & 4 != 0 ? .otherMouseDragged : .mouseMoved
                post(mouse: type, button: type == .rightMouseDragged ? .right : type == .otherMouseDragged ? .center : .left)
            }
            for bit in [1, 2, 4] where (old ^ b) & bit != 0 {
                buttons = (buttons & ~bit) | (b & bit)
                postButton(bit, down: b & bit != 0)
            }
            buttons = b
        case .scroll(let dy, let dx):
            scrollY += dy; scrollX += dx
            let ly = scrollY / 10, lx = scrollX / 10
            scrollY -= ly * 10; scrollX -= lx * 10
            guard ly != 0 || lx != 0,
                  let ev = CGEvent(scrollWheelEvent2Source: src, units: .line, wheelCount: 2,
                                   wheel1: Int32(ly), wheel2: Int32(lx), wheel3: 0) else { return }
            ev.flags = flags
            ev.post(tap: .cghidEventTap)
        }
    }

    // MARK: keys

    private static func modifierFlag(_ code: UInt16) -> CGEventFlags? {
        switch code {
        case 55, 54: return .maskCommand
        case 56, 60: return .maskShift
        case 58, 61: return .maskAlternate
        case 59, 62: return .maskControl
        case 63: return .maskSecondaryFn
        default: return nil
        }
    }

    private func setModifier(_ code: UInt16, down: Bool) {
        if code == 57 {                         // Caps Lock: each press toggles it
            guard down else { return }
            capsLock.toggle()
            if capsLock { flags.insert(.maskAlphaShift) } else { flags.remove(.maskAlphaShift) }
        } else if let f = Self.modifierFlag(code) {
            // still held by the other one of a left/right pair?
            let pairHeld = heldModifiers.contains { Self.modifierFlag($0) == f }
            if down || !pairHeld { if down { flags.insert(f) } else { flags.remove(f) } }
        }
        guard let ev = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: down) else { return }
        ev.type = .flagsChanged
        ev.flags = flags
        ev.post(tap: .cghidEventTap)
    }

    private func postKey(_ code: UInt16, down: Bool, isRepeat: Bool = false) {
        guard let ev = CGEvent(keyboardEventSource: src, virtualKey: code, keyDown: down) else { return }
        ev.flags = flags
        if isRepeat { ev.setIntegerValueField(.keyboardEventAutorepeat, value: 1) }
        ev.post(tap: .cghidEventTap)
    }

    private var repeating: UInt16?

    private func startRepeat(_ code: UInt16) {
        stopRepeat()
        repeating = code
        let t = DispatchSource.makeTimerSource(queue: q)
        t.schedule(deadline: .now() + repeatDelay, repeating: repeatInterval)
        t.setEventHandler { [weak self] in
            guard let self, self.repeating == code, self.held.contains(code) else { return }
            self.postKey(code, down: true, isRepeat: true)
        }
        t.resume()
        repeatTimer = t
    }

    private func stopRepeat() {
        repeatTimer?.cancel()
        repeatTimer = nil
        repeating = nil
    }

    // MARK: mouse

    private func postButton(_ bit: Int, down: Bool) {
        let (type, button): (CGEventType, CGMouseButton) =
            bit == 1 ? (down ? .leftMouseDown : .leftMouseUp, .left)
            : bit == 2 ? (down ? .rightMouseDown : .rightMouseUp, .right)
            : (down ? .otherMouseDown : .otherMouseUp, .center)
        if down {
            let now = nowMs()
            let near = abs(pos.x - lastDownPos.x) < 5 && abs(pos.y - lastDownPos.y) < 5
            clickCount = near && now - lastDownAt < NSEvent.doubleClickInterval * 1000 ? clickCount + 1 : 1
            lastDownAt = now
            lastDownPos = pos
        }
        post(mouse: type, button: button)
    }

    private func post(mouse type: CGEventType, button: CGMouseButton) {
        guard let ev = CGEvent(mouseEventSource: src, mouseType: type, mouseCursorPosition: pos, mouseButton: button) else { return }
        ev.flags = flags
        if type != .mouseMoved { ev.setIntegerValueField(.mouseEventClickState, value: Int64(clickCount)) }
        ev.post(tap: .cghidEventTap)
    }
}

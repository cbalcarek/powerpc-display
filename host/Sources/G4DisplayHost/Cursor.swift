import AppKit
import CCodec

/// Follows the mouse and sends tiny position messages; the PowerPC Mac draws
/// the cursor itself, so it stays smooth even while a big screen update is
/// still in flight. Driven by mouse-move events (nothing runs while the mouse
/// is still), plus a slow 10 Hz check for cursor-image changes without
/// movement (e.g. I-beam → arrow). The image is re-rendered only while the
/// pointer is on the PowerPC display, and sent only when it changes.
final class CursorTracker {
    private var timer: DispatchSourceTimer?
    private var monitors: [Any] = []
    private var lastShapeCheck = 0.0
    private weak var host: Host?
    private var displayID: CGDirectDisplayID = 0
    private var scale: CGFloat = 1, offX: CGFloat = 0, offY: CGFloat = 0
    private var last = (x: Int.min, y: Int.min, vis: false)
    private var lastShapeHash: UInt64 = 0

    /// G4 pixel = offset + (point within the display) * scale
    func start(host: Host, displayID: CGDirectDisplayID, scale: CGFloat, offsetX: CGFloat, offsetY: CGFloat) {
        stop()
        self.host = host
        self.displayID = displayID
        self.scale = scale
        self.offX = offsetX
        self.offY = offsetY
        last = (Int.min, Int.min, false)
        lastShapeHash = 0
        // mouse movement anywhere (global) and over our own windows (local);
        // mouse events don't need Accessibility permission
        let mask: NSEvent.EventTypeMask = [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged]
        if let g = NSEvent.addGlobalMonitorForEvents(matching: mask, handler: { [weak self] _ in self?.poll() }) {
            monitors.append(g)
        }
        if let l = NSEvent.addLocalMonitorForEvents(matching: mask, handler: { [weak self] e in self?.poll(); return e }) {
            monitors.append(l)
        }
        // any input (no Accessibility needed for these): adaptive pacing
        let input: NSEvent.EventTypeMask = [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged,
                                            .scrollWheel, .leftMouseDown, .rightMouseDown]
        if let g = NSEvent.addGlobalMonitorForEvents(matching: input, handler: { [weak self] _ in self?.host?.noteInput() }) {
            monitors.append(g)
        }
        if let l = NSEvent.addLocalMonitorForEvents(matching: input, handler: { [weak self] e in self?.host?.noteInput(); return e }) {
            monitors.append(l)
        }
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now(), repeating: .milliseconds(100), leeway: .milliseconds(20))
        t.setEventHandler { [weak self] in self?.poll() }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel()
        timer = nil
        for m in monitors { NSEvent.removeMonitor(m) }
        monitors.removeAll()
    }

    private func poll() {
        guard let host, let loc = CGEvent(source: nil)?.location else { return }
        let b = CGDisplayBounds(displayID)
        var x = 0, y = 0, vis = false
        if b.width > 0, b.contains(loc) {
            x = Int((offX + (loc.x - b.minX) * scale).rounded(.down))
            y = Int((offY + (loc.y - b.minY) * scale).rounded(.down))
            vis = true
        }
        if x != last.x || y != last.y || vis != last.vis {
            // while off-screen, only the hide message matters
            if vis || last.vis || last.x == Int.min { host.sendCursorPos(x: x, y: y, visible: vis) }
            last = (x, y, vis)
        }

        let now = nowMs()
        if vis && (now - lastShapeCheck >= 50 || lastShapeHash == 0) {
            lastShapeCheck = now
            updateShape(host)
        }
    }

    private func updateShape(_ host: Host) {
        guard let cursor = NSCursor.currentSystem else { return }
        let img = cursor.image
        // mirror mode shrinks the picture, so shrink the pointer to match
        let k = min(1, scale)
        let w = Int((img.size.width * k).rounded(.up)), h = Int((img.size.height * k).rounded(.up))
        guard w > 0, h > 0, w <= 256, h <= 256 else { return }

        let bytes = w * h * 4
        let buf = UnsafeMutableRawPointer.allocate(byteCount: bytes, alignment: 16)
        defer { buf.deallocate() }
        memset(buf, 0, bytes)
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let ctx = CGContext(data: buf, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: space,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        else { return }
        NSGraphicsContext.saveGraphicsState()
        NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
        img.draw(in: NSRect(x: 0, y: 0, width: w, height: h))
        NSGraphicsContext.restoreGraphicsState()

        // BGRA (premultiplied) -> A,R,G,B bytes the G4's GL wants
        var argb = [UInt8](repeating: 0, count: bytes)
        argb.withUnsafeMutableBufferPointer { dst in
            g4_bgra_to_wire32(buf.assumingMemoryBound(to: UInt8.self), dst.baseAddress, w * h)
        }
        var hash: UInt64 = 0xcbf29ce484222325 ^ UInt64(w << 16 | h)
        for b in argb { hash = (hash ^ UInt64(b)) &* 0x100000001b3 }
        let hot = CGPoint(x: cursor.hotSpot.x * k, y: cursor.hotSpot.y * k)
        hash ^= UInt64(truncatingIfNeeded: Int(hot.x) << 8 | Int(hot.y))
        guard hash != lastShapeHash else { return }
        lastShapeHash = hash
        host.sendCursorShape(width: w, height: h,
                             hotX: min(max(Int(hot.x), 0), w - 1), hotY: min(max(Int(hot.y), 0), h - 1),
                             argb: argb)
    }
}

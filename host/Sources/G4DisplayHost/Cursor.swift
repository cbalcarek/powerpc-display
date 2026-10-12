import AppKit
import CCodec

/// Polls the mouse at ~125 Hz on the main thread and sends tiny position
/// messages. The G4 draws the cursor itself, so it stays smooth even while a
/// big screen update is still in flight. The cursor image is re-rendered only
/// while the pointer is on the G4 display, and sent only when it changes.
final class CursorTracker {
    private var timer: DispatchSourceTimer?
    private weak var host: Host?
    private var displayID: CGDirectDisplayID = 0
    private var scale: CGFloat = 1, offX: CGFloat = 0, offY: CGFloat = 0
    private var last = (x: Int.min, y: Int.min, vis: false)
    private var lastShapeHash: UInt64 = 0
    private var tick = 0

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
        let t = DispatchSource.makeTimerSource(queue: .main)
        t.schedule(deadline: .now(), repeating: .milliseconds(8), leeway: .milliseconds(1))
        t.setEventHandler { [weak self] in self?.poll() }
        t.resume()
        timer = t
    }

    func stop() {
        timer?.cancel()
        timer = nil
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

        tick += 1
        if vis && (tick % 6 == 0 || lastShapeHash == 0) { updateShape(host) }
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

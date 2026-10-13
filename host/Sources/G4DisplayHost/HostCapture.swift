import AppKit
import CCodec
import CoreMedia
import CoreVideo
import CVirtualDisplay
import Foundation
import ScreenCaptureKit

/// Virtual display and ScreenCaptureKit capture.
extension Host {
    /// Create (or reuse) the virtual display matching the PowerPC Mac's screen.
    /// Connection thread.
    func setUpDisplay() -> Bool {
        if let fixed = cfg.fixedDisplayID {
            displayID = fixed
            return CGDisplayBounds(fixed).width > 0
        }
        let label = displayLabel
        if let vd = vdisplay, vdisplayName == label,
           CGDisplayPixelsWide(vd.displayID) == W, CGDisplayPixelsHigh(vd.displayID) == H {
            displayID = vd.displayID
            return true
        }
        var created: G4VirtualDisplay?
        let w = W, h = H
        // physical size: what the PowerPC Mac reports, else assume ~100 pixels per inch
        let mmW = widthMM > 0 ? Double(widthMM) : Double(W) * 0.254
        let mmH = mmW * Double(H) / Double(max(W, 1))
        DispatchQueue.main.sync {
            self.vdisplay = nil   // drop any old one first
            created = G4VirtualDisplay(width: UInt32(w), height: UInt32(h), refreshRate: 60, name: label,
                                       widthMM: mmW, heightMM: mmH)
        }
        guard let vd = created else { return false }
        vdisplay = vd
        vdisplayName = label
        displayID = vd.displayID
        log("virtual display \(displayID) created (\(W)x\(H)) — arrange it in System Settings › Displays")
        return true
    }

    /// Remove the virtual display (it disappears from System Settings › Displays).
    func releaseDisplay() {
        guard vdisplay != nil else { return }
        DispatchQueue.main.sync { self.vdisplay = nil }
    }

    func startCapture() async throws {
        // a freshly created display can take a moment to show up
        var scDisplay: SCDisplay?
        for _ in 0..<20 {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            scDisplay = content.displays.first { $0.displayID == displayID }
            if scDisplay != nil { break }
            try await Task.sleep(nanoseconds: 250_000_000)
        }
        guard let display = scDisplay else {
            throw NSError(domain: "PowerPCDisplay", code: 1, userInfo: [NSLocalizedDescriptionKey: "display \(displayID) not capturable"])
        }

        let sc = SCStreamConfiguration()
        sc.width = W
        sc.height = H
        sc.pixelFormat = kCVPixelFormatType_32BGRA
        sc.showsCursor = false                  // the PowerPC Mac draws the cursor itself
        sc.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        sc.queueDepth = 4
        sc.colorSpaceName = CGColorSpace.sRGB
        // map display points -> PowerPC pixels. Extend: the virtual display is
        // the PowerPC Mac's size. Mirror: scale the main display and letterbox it.
        let dw = CGFloat(max(display.width, 1)), dh = CGFloat(max(display.height, 1))
        let mirroring = isMirror
        var scale: CGFloat = 1, offX: CGFloat = 0, offY: CGFloat = 0
        if mirroring {
            sc.scalesToFit = true
            sc.preservesAspectRatio = true
            // letterbox bars are black by default. (Don't set backgroundColor:
            // SCStreamConfiguration doesn't retain it, so a temporary CGColor
            // is freed and the stream crashes copying it.)
            scale = min(CGFloat(W) / dw, CGFloat(H) / dh)
            offX = ((CGFloat(W) - dw * scale) / 2).rounded(.down)
            offY = ((CGFloat(H) - dh * scale) / 2).rounded(.down)
        } else {
            scale = CGFloat(W) / dw
        }
        encQ.sync {
            hintScale = scale; hintOffX = offX; hintOffY = offY
            // converting only the changed rectangles is safe when the
            // captured frame is the display 1:1 (extend mode); a scaled mirror
            // smears changes across neighbouring pixels, so convert everything there
            rectIngest = !mirroring && abs(scale - 1) < 0.001
        }

        let filter = SCContentFilter(display: display, excludingWindows: [])
        let s = SCStream(filter: filter, configuration: sc, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: encQ)
        try await s.startCapture()
        lock.lock(); _stream = s; _scConfig = sc; lock.unlock()
    }

    // MARK: - frames (encQ)

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .screen, let enc else { return }
        guard let arr = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
              let att = arr.first,
              let rawStatus = att[.status] as? Int,
              SCFrameStatus(rawValue: rawStatus) == .complete,
              let pb = CMSampleBufferGetImageBuffer(sb)
        else { return }   // idle/blank frames carry nothing new

        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        guard CVPixelBufferGetWidth(pb) == W, CVPixelBufferGetHeight(pb) == H,
              let base = CVPixelBufferGetBaseAddress(pb) else {
            needFullIngest = true          // whatever changed in this frame, catch it next time
            return
        }
        let src = base.assumingMemoryBound(to: UInt8.self)
        let stride = CVPixelBufferGetBytesPerRow(pb)
        let rects = Host.dirtyRects(att[.dirtyRects])

        // Convert only what ScreenCaptureKit says changed; the whole screen on
        // the first frame, when unsure, and once a second as a safety net.
        let now = nowMs()
        if needFullIngest || !rectIngest || rects.isEmpty || now - lastFullIngest > 1000 {
            g4enc_ingest_bgra(enc, src, stride)
            needFullIngest = false
            lastFullIngest = now
        } else {
            for r in rects {
                let x = Int(r.minX.rounded(.down)) - 1, y = Int(r.minY.rounded(.down)) - 1
                let w = Int(r.width.rounded(.up)) + 2, h = Int(r.height.rounded(.up)) + 2
                g4enc_ingest_bgra_rect(enc, src, stride, Int32(x), Int32(y), Int32(w), Int32(h))
            }
        }

        for r in rects {
            g4enc_add_hint(enc, Int32(hintOffX + r.minX * hintScale), Int32(hintOffY + r.minY * hintScale),
                           Int32((r.width * hintScale).rounded(.up)), Int32((r.height * hintScale).rounded(.up)))
        }
        pending = true
        trySend()
    }

    /// ScreenCaptureKit's dirty rects arrive as CFDictionary-encoded CGRects;
    /// accept the other plausible bridgings too.
    static func dirtyRects(_ value: Any?) -> [CGRect] {
        guard let arr = value as? [Any] else { return [] }
        return arr.compactMap { item in
            if CFGetTypeID(item as CFTypeRef) == CFDictionaryGetTypeID() {
                return CGRect(dictionaryRepresentation: item as! CFDictionary)
            }
            if let v = item as? NSValue { return v.rectValue }
            return nil
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        log("capture stopped: \(error.localizedDescription)")
        dropConnection()
    }
}

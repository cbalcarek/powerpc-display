import AppKit
import CCodec
import CDiscovery
import CoreMedia
import CoreVideo
import CVirtualDisplay
import Foundation
import ScreenCaptureKit

/// Owns one connection to the G4 at a time: virtual display, capture,
/// encoder, flow control and the cursor channel.
///
/// Threading: everything that touches the encoder or writes to the socket
/// runs on `encQ` (ScreenCaptureKit delivers frames straight onto it), so
/// there are no locks on the hot path. A connection thread runs the
/// discover → connect → stream loop and blocks on socket reads (ACKs).
/// Settings changed from the menu bar go through `lock`.
final class Host: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    struct Config {
        var g4Address: String? = nil      // nil = find the G4 with Bonjour
        var g4Name: String? = nil         // Bonjour: only this computer name
        var port: UInt16 = UInt16(G4_PORT)
        var fixedDisplayID: CGDirectDisplayID? = nil   // use an existing display (e.g. BetterDisplay)
        var fps: Int = 60
        var maxInFlight: UInt32 = 2
        var verbose = false
        var mirror = false                     // show the main display instead of a new one
    }

    enum Status: Equatable {
        case searching(String?)                 // address being tried, or nil = Bonjour
        case connected(name: String, width: Int, height: Int, depth: Int)
        case paused
        case needsScreenRecording
        case failed(String)
        case cantReach(String)                  // found it, but something in the way
    }

    struct Stats {
        var fps = 0.0, kbps = 0.0, latencyMs = 0.0, compression = 0.0
        var at = 0.0                            // nowMs() when measured
    }

    let cfg: Config
    let encQ = DispatchQueue(label: "g4.encode", qos: .userInteractive)
    let cursor = CursorTracker()

    /// Called on the main thread.
    var onStatus: ((Status) -> Void)?
    var onStats: ((Stats) -> Void)?

    // --- settings shared with the UI (guarded by lock) ---
    private let lock = NSLock()
    private var _enabled = true
    private var _fps: Int
    private var _mirror: Bool
    private var _stream: SCStream?
    private var _scConfig: SCStreamConfiguration?
    private let wake = DispatchSemaphore(value: 0)
    private var lastStatus: Status?
    private var lastDiscoveryNote = ""

    // --- state owned by encQ ---
    private var fd: Int32 = -1
    private var enc: OpaquePointer?
    private var sentID: UInt32 = 0
    private var ackedID: UInt32 = 0
    private var pending = false
    private var sendTimes: [UInt32: Double] = [:]
    private var hintScale: CGFloat = 1
    private var hintOffX: CGFloat = 0, hintOffY: CGFloat = 0   // letterbox offsets (mirror mode)

    // --- per-connection (connection thread) ---
    private var W = 0, H = 0, depth = 32, slots = 0, widthMM = 0
    private var peerName = ""
    /// where the PowerPC Mac was last reached: tried first on reconnect (e.g.
    /// after switching Extend/Mirror), before asking Bonjour again
    private var lastEndpoint: (host: String, port: UInt16, name: String)?
    private var vdisplayName = ""
    private var vdisplay: G4VirtualDisplay?
    private var displayID: CGDirectDisplayID = 0

    // --- stats (encQ) ---
    private var statT0 = nowMs()
    private var statFrames = 0, statBytes = 0, statRaw = 0, statBlocked = 0, statCopies = 0, statHits = 0
    private var statRtt = 0.0, statRttN = 0

    init(config: Config) {
        cfg = config
        _fps = config.fps
        _mirror = config.mirror
        super.init()
    }

    func start() {
        let t = Thread { [weak self] in self?.connectLoop() }
        t.qualityOfService = .userInitiated
        t.start()
    }

    // MARK: - settings (any thread)

    var isEnabled: Bool { lock.lock(); defer { lock.unlock() }; return _enabled }
    var fps: Int { lock.lock(); defer { lock.unlock() }; return _fps }

    /// Pause/resume. Pausing drops the connection and removes the virtual display.
    func setEnabled(_ on: Bool) {
        lock.lock(); _enabled = on; lock.unlock()
        if on {
            wake.signal()
        } else {
            encQ.async { if self.fd >= 0 { shutdown(self.fd, SHUT_RDWR) } }
        }
    }

    var isMirror: Bool { lock.lock(); defer { lock.unlock() }; return _mirror }

    /// Switch between extending (a new virtual display) and mirroring the main
    /// display. Applied by dropping the connection; the G4 reconnects at once.
    func setMirror(_ on: Bool) {
        lock.lock(); let changed = _mirror != on; _mirror = on; lock.unlock()
        guard changed else { return }
        encQ.async { if self.fd >= 0 { shutdown(self.fd, SHUT_RDWR) } }
    }

    func setFPS(_ value: Int) {
        lock.lock()
        _fps = max(1, min(120, value))
        let s = _stream, conf = _scConfig, f = _fps
        lock.unlock()
        guard let s, let conf else { return }
        conf.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(f))
        Task { try? await s.updateConfiguration(conf) }
    }

    private func post(_ s: Status) {
        guard s != lastStatus else { return }
        lastStatus = s
        DispatchQueue.main.async { self.onStatus?(s) }
    }

    // MARK: - connection lifecycle

    private func connectLoop() {
        var announced = false
        while true {
            if !isEnabled {
                post(.paused)
                releaseDisplay()
                while !isEnabled { _ = wake.wait(timeout: .now() + 1) }
                announced = false
            }
            guard let conn = connectToG4(announced: &announced) else {
                sleep(cfg.g4Address == nil ? 1 : 2)
                continue
            }
            let (sock, name) = conn
            peerName = name
            if !isEnabled { close(sock); continue }
            announced = false
            guard let hello = readHello(sock) else {
                log("bad hello"); lastEndpoint = nil; close(sock); sleep(1); continue
            }
            (W, H, depth, slots, widthMM) = hello
            log("\(name) connected: \(W)x\(H), \(depth)-bit, \(slots) cache tiles\(widthMM > 0 ? ", \(widthMM) mm wide" : "")")

            let mirroring = isMirror
            if mirroring {
                releaseDisplay()                  // no extra display while mirroring
                displayID = CGMainDisplayID()
                log("mirroring the main display (\(displayID)) at \(W)x\(H)")
            }
            guard mirroring || setUpDisplay() else {
                log("could not create/find the display")
                post(.failed("Couldn't create the virtual display"))
                close(sock); sleep(3); continue
            }
            encQ.sync {
                fd = sock
                if let e = enc { g4enc_destroy(e) }
                enc = g4enc_create(Int32(W), Int32(H), Int32(depth))
                if let e = enc, slots > 0, g4enc_enable_cache(e, Int32(slots)) != 0 {
                    log("tile cache disabled (out of memory)")
                }
                sentID = 0; ackedID = 0; pending = false
                sendTimes.removeAll()
            }

            let started = runAsync { try await self.startCapture() }
            if !started {
                if CGPreflightScreenCaptureAccess() {
                    log("screen capture failed")
                    post(.failed("Screen capture failed"))
                } else {
                    log("screen capture needs Screen Recording permission — waiting (no more reconnects)")
                    post(.needsScreenRecording)
                }
                teardown(sock)
                // don't keep reconnecting (each attempt would blank the G4's
                // screen); wait until permission is granted
                while !CGPreflightScreenCaptureAccess() && isEnabled { sleep(2) }
                sleep(1)
                continue
            }
            post(.connected(name: name, width: W, height: H, depth: depth))
            let did = displayID, sc = hintScale, ox = hintOffX, oy = hintOffY
            DispatchQueue.main.async {
                self.cursor.start(host: self, displayID: did, scale: sc, offsetX: ox, offsetY: oy)
            }

            readerLoop(sock)      // blocks until the G4 goes away (or we pause)
            teardown(sock)
            log("\(name) disconnected")
            sleep(1)
        }
    }

    /// Name shown in System Settings › Displays: the PowerPC Mac's computer name.
    private var displayLabel: String {
        let n = peerName
        if n.isEmpty || n.first?.isNumber == true || n.contains(":") || n == "localhost" { return "PowerPC Display" }
        return n
    }

    /// Connect by address, or find the PowerPC Mac on the network with Bonjour.
    private func connectToG4(announced: inout Bool) -> (Int32, String)? {
        if let addr = cfg.g4Address {
            if let s = connectTCP(host: addr, port: cfg.port) { return (s, addr) }
            if !announced { log("waiting for the PowerPC Mac at \(addr):\(cfg.port)…"); announced = true }
            post(.searching(addr))
            return nil
        }
        if let ep = lastEndpoint {
            if let s = connectTCP(host: ep.host, port: ep.port, timeoutMs: 1000) {
                log("reconnected to \"\(ep.name)\" at \(ep.host):\(ep.port)")
                return (s, ep.name)
            }
            lastEndpoint = nil      // gone or moved: find it again with Bonjour
        }
        var found = g4_found()
        let rc: Int32
        if let wanted = cfg.g4Name {
            rc = wanted.withCString { g4_discover($0, 3000, &found) }
        } else {
            rc = g4_discover(nil, 3000, &found)
        }
        let name = cString(&found.name)
        let host = cString(&found.host)

        if rc != 0 {
            // Bonjour saw it but couldn't finish the lookup: a VM in UTM's
            // Emulated VLAN mode is reachable through UTM's port forward on
            // this Mac, so try that before reporting a problem
            if found.failed_step >= 2,
               let s = connectTCP(host: "127.0.0.1", port: UInt16(G4_PORT), timeoutMs: 500) {
                log("found \"\(name)\"; Bonjour lookup failed, connected through 127.0.0.1:\(G4_PORT) (UTM port forward)")
                lastDiscoveryNote = ""
                lastEndpoint = ("127.0.0.1", UInt16(G4_PORT), name)
                return (s, name)
            }
            switch found.failed_step {
            case 1 where found.dns_error == 0:
                // nothing on the network yet: normal while the PowerPC Mac isn't running
                if !announced { log("looking for the PowerPC Mac on the network (Bonjour)…"); announced = true }
                post(.searching(nil))
            case 1:
                note("Bonjour browsing failed (error \(found.dns_error))",
                     found.dns_error == -65570
                        ? "macOS blocked PowerPC Display from browsing the local network. Turn it on in System Settings › Privacy & Security › Local Network, then quit and reopen."
                        : "Bonjour browsing failed (error \(found.dns_error)).")
            case 2:
                note("found \"\(name)\" but could not resolve it (error \(found.dns_error))",
                     "Found “\(name)” but couldn't get its details (Bonjour error \(found.dns_error)).")
            default:
                // address lookup failed: fall back to the system resolver on the host name
                if !host.isEmpty, let s = connectTCP(host: host, port: found.port) {
                    log("found \"\(name)\" at \(host):\(found.port) (system resolver)")
                    lastDiscoveryNote = ""
                    lastEndpoint = (host, found.port, name)
                    return (s, name)
                }
                note("found \"\(name)\" (\(host)) but could not look up its address (error \(found.dns_error))",
                     "Found “\(name)” but couldn't look up its network address.")
            }
            return nil
        }

        let ip = cString(&found.ip)
        if lastDiscoveryNote.isEmpty { log("found \"\(name)\" at \(ip):\(found.port), connecting…") }
        // last resort: a VM in UTM's Emulated VLAN mode advertises an internal
        // address (10.0.2.x) the Mac can't reach; its port forward is on localhost
        var via = ip
        var sock = connectTCP(host: ip, port: found.port, ifindex: found.ifindex) ?? connectTCP(host: ip, port: found.port)
        if sock == nil {
            sock = connectTCP(host: "127.0.0.1", port: found.port, timeoutMs: 1000)
            via = "127.0.0.1"
        }
        guard let s = sock else {
            note("found \"\(name)\" at \(ip):\(found.port) but could not connect",
                 "Found “\(name)” at \(ip), but the connection was refused or blocked. Check that nothing between the two Macs blocks port \(found.port).")
            return nil
        }
        log("connected to \"\(name)\" at \(via):\(found.port)")
        lastDiscoveryNote = ""
        lastEndpoint = (via, found.port, name)
        return (s, name)
    }

    /// Log a discovery problem once, and show it in the window.
    private func note(_ logLine: String, _ userText: String) {
        if logLine != lastDiscoveryNote {
            log(logLine)
            lastDiscoveryNote = logLine
        }
        post(.cantReach(userText))
    }

    private func teardown(_ sock: Int32) {
        DispatchQueue.main.sync { self.cursor.stop() }
        lock.lock()
        let s = _stream
        _stream = nil; _scConfig = nil
        lock.unlock()
        if let s {
            _ = runAsync { try await s.stopCapture() }
        }
        encQ.sync {
            close(sock)
            fd = -1
            if let e = enc { g4enc_destroy(e) }
            enc = nil
        }
    }

    /// Remove the virtual display (it disappears from System Settings › Displays).
    private func releaseDisplay() {
        guard vdisplay != nil else { return }
        DispatchQueue.main.sync { self.vdisplay = nil }
    }

    /// Runs async work from a plain thread and waits for it. Returns false on error.
    private func runAsync(_ body: @escaping @Sendable () async throws -> Void) -> Bool {
        let sem = DispatchSemaphore(value: 0)
        let ok = UnsafeMutablePointer<Bool>.allocate(capacity: 1)
        defer { ok.deallocate() }
        ok.pointee = true
        Task.detached {
            do { try await body() } catch {
                log("error: \(error.localizedDescription)")
                ok.pointee = false
            }
            sem.signal()
        }
        sem.wait()
        return ok.pointee
    }

    private func readHello(_ sock: Int32) -> (Int, Int, Int, Int, Int)? {
        var hdr = [UInt8](repeating: 0, count: 8)
        guard readAll(sock, &hdr, 8), Int(hdr[0]) == G4_MSG_HELLO else { return nil }
        let len = Int(be32(hdr, 4))
        guard len >= 12, len < 1024 else { return nil }
        var p = [UInt8](repeating: 0, count: len)
        guard readAll(sock, &p, len), p[0] == 0x47, p[1] == 0x34, p[2] == 0x44, p[3] == 0x50 else { return nil }
        let w = be16(p, 6), h = be16(p, 8), d = Int(p[10])
        let cacheSlots = len >= 14 ? be16(p, 12) : 0
        let mm = len >= 16 ? be16(p, 14) : 0          // physical width; 0 = unknown (older clients)
        guard w > 0, h > 0, d == 16 || d == 32 else { return nil }
        return (w, h, d, cacheSlots, mm)
    }

    private func setUpDisplay() -> Bool {
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

    private func startCapture() async throws {
        // a freshly created display can take a moment to show up
        var scDisplay: SCDisplay?
        for _ in 0..<20 {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            scDisplay = content.displays.first { $0.displayID == displayID }
            if scDisplay != nil { break }
            try await Task.sleep(nanoseconds: 250_000_000)
        }
        guard let display = scDisplay else {
            throw NSError(domain: "g4host", code: 1, userInfo: [NSLocalizedDescriptionKey: "display \(displayID) not capturable"])
        }

        let sc = SCStreamConfiguration()
        sc.width = W
        sc.height = H
        sc.pixelFormat = kCVPixelFormatType_32BGRA
        sc.showsCursor = false                  // the G4 draws the cursor itself
        sc.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(fps))
        sc.queueDepth = 4
        sc.colorSpaceName = CGColorSpace.sRGB
        // map display points -> G4 pixels. Extend: the virtual display is the
        // G4's size. Mirror: scale the main display down and letterbox it.
        let dw = CGFloat(max(display.width, 1)), dh = CGFloat(max(display.height, 1))
        if isMirror {
            sc.scalesToFit = true
            sc.preservesAspectRatio = true
            sc.backgroundColor = CGColor(gray: 0, alpha: 1)
            hintScale = min(CGFloat(W) / dw, CGFloat(H) / dh)
            hintOffX = ((CGFloat(W) - dw * hintScale) / 2).rounded(.down)
            hintOffY = ((CGFloat(H) - dh * hintScale) / 2).rounded(.down)
        } else {
            hintScale = CGFloat(W) / dw
            hintOffX = 0; hintOffY = 0
        }

        let filter = SCContentFilter(display: display, excludingWindows: [])
        let s = SCStream(filter: filter, configuration: sc, delegate: self)
        try s.addStreamOutput(self, type: .screen, sampleHandlerQueue: encQ)
        try await s.startCapture()
        lock.lock(); _stream = s; _scConfig = sc; lock.unlock()
    }

    // MARK: - capture → encode → send  (encQ)

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
              let base = CVPixelBufferGetBaseAddress(pb) else { return }

        g4enc_ingest_bgra(enc, base.assumingMemoryBound(to: UInt8.self), CVPixelBufferGetBytesPerRow(pb))

        for r in Host.dirtyRects(att[.dirtyRects]) {
            g4enc_add_hint(enc, Int32(hintOffX + r.minX * hintScale), Int32(hintOffY + r.minY * hintScale),
                           Int32((r.width * hintScale).rounded(.up)), Int32((r.height * hintScale).rounded(.up)))
        }
        pending = true
        trySend()
    }

    /// ScreenCaptureKit's dirty rects arrive as CFDictionary-encoded CGRects;
    /// accept the other plausible bridgings too.
    private static func dirtyRects(_ value: Any?) -> [CGRect] {
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
        encQ.async { if self.fd >= 0 { shutdown(self.fd, SHUT_RDWR) } }
    }

    /// Encode whatever changed since the G4's last known state — but only if
    /// fewer than `maxInFlight` frames are unacknowledged. Otherwise wait: the
    /// next ACK calls this again and everything that changed meanwhile goes
    /// out as one coalesced frame. This is what keeps lag low.
    private func trySend() {
        guard pending, fd >= 0, let enc else { return }
        if sentID &- ackedID >= cfg.maxInFlight { statBlocked += 1; return }

        var out: UnsafePointer<UInt8>?
        var st = g4_enc_stats()
        g4enc_set_time(enc, UInt32(truncatingIfNeeded: Int64(nowMs())))
        let n = g4enc_encode(enc, sentID &+ 1, &out, &st)
        pending = false
        guard n > 0, let out else { return }

        if writeAll(fd, out, n) {
            sentID &+= 1
            sendTimes[sentID] = nowMs()
            statFrames += 1; statBytes += n; statRaw += Int(st.raw_bytes); statCopies += Int(st.copies)
            statHits += Int(st.cache_hits)
        } else {
            shutdown(fd, SHUT_RDWR)
        }
        reportStats()
    }

    private func onAck(_ id: UInt32) {
        guard id > ackedID, id <= sentID else { return }
        if let t = sendTimes[id] { statRtt += nowMs() - t; statRttN += 1 }
        for k in sendTimes.keys where k <= id { sendTimes[k] = nil }
        ackedID = id
        trySend()
    }

    private func reportStats() {
        let dt = nowMs() - statT0
        guard dt > 2000 else { return }
        let s = dt / 1000
        var snap = Stats()
        snap.fps = Double(statFrames) / s
        snap.kbps = Double(statBytes) / 1024 / s
        snap.compression = statBytes > 0 ? Double(statRaw) / Double(statBytes) : 0
        snap.latencyMs = statRttN > 0 ? statRtt / Double(statRttN) : 0
        snap.at = nowMs()
        DispatchQueue.main.async { self.onStats?(snap) }
        if cfg.verbose {
            log(String(format: "%.1f fps  %.0f KB/s  (%.1fx)  frame→screen %.1f ms  waits %d  scroll copies %d  cache hits %d",
                       snap.fps, snap.kbps, snap.compression, snap.latencyMs, statBlocked, statCopies, statHits))
        }
        statT0 = nowMs(); statFrames = 0; statBytes = 0; statRaw = 0; statBlocked = 0
        statCopies = 0; statHits = 0; statRtt = 0; statRttN = 0
    }

    // MARK: - reading (connection thread)

    private func readerLoop(_ sock: Int32) {
        var hdr = [UInt8](repeating: 0, count: 8)
        var payload = [UInt8](repeating: 0, count: 64)
        while readAll(sock, &hdr, 8) {
            let len = Int(be32(hdr, 4))
            if len > 1 << 20 { break }
            if len > payload.count { payload = [UInt8](repeating: 0, count: len) }
            if len > 0, !readAll(sock, &payload, len) { break }
            if Int(hdr[0]) == G4_MSG_ACK, len >= 4 {
                let id = be32(payload, 0)
                encQ.async { self.onAck(id) }
            }
        }
    }

    // MARK: - cursor channel (called from the main thread)

    func sendCursorPos(x: Int, y: Int, visible: Bool) {
        var p = [UInt8](repeating: 0, count: 8)
        put16(&p, 0, x); put16(&p, 2, y); p[4] = visible ? 1 : 0
        let m = message(type: G4_MSG_CURSOR_POS, payload: p)
        encQ.async {
            guard self.fd >= 0 else { return }
            if !writeAll(self.fd, m, m.count) { shutdown(self.fd, SHUT_RDWR) }
        }
    }

    func sendCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, argb: [UInt8]) {
        var p = [UInt8](repeating: 0, count: 8)
        put16(&p, 0, width); put16(&p, 2, height); put16(&p, 4, hotX); put16(&p, 6, hotY)
        let m = message(type: G4_MSG_CURSOR_SHAPE, payload: p + argb)
        encQ.async {
            guard self.fd >= 0 else { return }
            if !writeAll(self.fd, m, m.count) { shutdown(self.fd, SHUT_RDWR) }
        }
    }
}

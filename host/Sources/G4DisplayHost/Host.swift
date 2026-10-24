import AppKit
import CCodec
import CDiscovery
import CoreMedia
import CoreVideo
import CVirtualDisplay
import Foundation
import ScreenCaptureKit

/// Owns one connection to the PowerPC Mac at a time.
///
/// Files:
///   Host.swift          state, settings, connect → pair → stream lifecycle
///   HostDiscovery.swift finding the PowerPC Mac (remembered address, Bonjour)
///   Pairing.swift       HELLO, pairing and the per-connection proof
///   HostCapture.swift   virtual display, ScreenCaptureKit, frames → encoder
///   HostSender.swift    encode/flow control, pings, ACKs, cursor messages, stats
///
/// Threading (who owns what):
///   connection thread  – the connect loop: discovery, handshake, display
///                        setup, then blocks reading ACK/PONG until disconnect.
///                        Owns the "per-connection" and "discovery" state.
///   encQ               – everything that touches the encoder or writes to the
///                        socket: frames from ScreenCaptureKit arrive on it,
///                        cursor messages and pings are queued onto it.
///   main               – UI callbacks, the virtual display object, cursor tracking.
///   `lock`             – only the settings the UI changes.
final class Host: NSObject, SCStreamOutput, SCStreamDelegate, @unchecked Sendable {
    struct Config {
        var g4Address: String? = nil      // nil = find the PowerPC Mac with Bonjour
        var g4Name: String? = nil         // Bonjour: only this computer name
        var port: UInt16 = UInt16(G4_PORT)
        var fixedDisplayID: CGDirectDisplayID? = nil   // use an existing display (e.g. BetterDisplay)
        var fps: Int = 60
        var maxInFlight: UInt32 = 2
        var verbose = false
        var mirror = false                     // show the main display instead of a new one
        var smoothVideo = true                 // video regions as 4:2:2 (half the data, softer)
        var fastVideo = true                   // ...and at half resolution (a quarter of that)
    }

    enum Status: Equatable {
        case searching(String?)                 // address being tried, or nil = Bonjour
        case pairing(name: String, code: UInt32, waitingForOtherMac: Bool = false)
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
    let encQ = DispatchQueue(label: "ppcdisplay.encode", qos: .userInteractive)
    let cursor = CursorTracker()

    /// Called on the main thread.
    var onStatus: ((Status) -> Void)?
    var onStats: ((Stats) -> Void)?

    // --- settings shared with the UI (guarded by lock) ---
    let lock = NSLock()
    var _enabled = true
    var _fps: Int
    var _mirror: Bool
    var _smoothVideo: Bool
    var _fastVideo: Bool
    var _stream: SCStream?
    var _scConfig: SCStreamConfiguration?
    let wake = DispatchSemaphore(value: 0)
    var lastStatus: Status?                     // post() may be called from any thread
    let statusLock = NSLock()

    // --- state owned by encQ ---
    var fd: Int32 = -1
    var enc: OpaquePointer?
    var clientYUV = false                      // the connected client can decode 4:2:2 rects
    var clientHalfVideo = false                // ...and half-resolution ones (0.11+)
    var refreshScheduled = false
    var sentID: UInt32 = 0
    var ackedID: UInt32 = 0
    var pending = false
    var sendTimes: [UInt32: Double] = [:]
    var hintScale: CGFloat = 1
    var hintOffX: CGFloat = 0, hintOffY: CGFloat = 0   // letterbox offsets (mirror mode)
    var lastHeard = 0.0                        // last ACK/PONG from the PowerPC Mac
    var pingTimer: DispatchSourceTimer?
    var pingSeq: UInt32 = 0
    var needFullIngest = true                  // next frame: convert the whole screen
    var lastFullIngest = 0.0
    var rectIngest = false                     // dirty-rect ingestion allowed (extend mode)

    // --- per-connection (connection thread) ---
    var W = 0, H = 0, depth = 32, slots = 0, widthMM = 0
    var peerName = ""
    var vdisplayName = ""
    var vdisplay: G4VirtualDisplay?
    var displayID: CGDirectDisplayID = 0

    // --- discovery (connection thread) ---
    var lastDiscoveryNote = ""
    /// where the PowerPC Mac was last reached (also across launches): tried
    /// first, before asking Bonjour
    var lastEndpoint: (host: String, port: UInt16, name: String)?
    /// after a bad handshake there (e.g. UTM's port forward with nothing
    /// behind it), leave the remembered address alone for a while
    var endpointSkipUntil: Double = 0
    /// PowerPC Macs the user said no to (until the app is reopened)
    var declinedPairing = Set<String>()

    // --- stats (encQ) ---
    var statT0 = nowMs()
    var statFrames = 0, statBytes = 0, statRaw = 0, statBlocked = 0, statCopies = 0, statHits = 0
    var statYuv = 0, statRefresh = 0
    var statRtt = 0.0, statRttN = 0

    init(config: Config) {
        cfg = config
        _fps = config.fps
        _mirror = config.mirror
        _smoothVideo = config.smoothVideo
        _fastVideo = config.fastVideo
        super.init()
        loadRememberedEndpoint()
    }

    func start() {
        let t = Thread { [weak self] in self?.connectLoop() }
        t.qualityOfService = .userInitiated
        t.start()
    }

    // MARK: - settings (any thread)

    var isEnabled: Bool { lock.lock(); defer { lock.unlock() }; return _enabled }
    var fps: Int { lock.lock(); defer { lock.unlock() }; return _fps }
    var isMirror: Bool { lock.lock(); defer { lock.unlock() }; return _mirror }
    var smoothVideo: Bool { lock.lock(); defer { lock.unlock() }; return _smoothVideo }
    var fastVideo: Bool { lock.lock(); defer { lock.unlock() }; return _fastVideo }

    /// Pause/resume. Pausing drops the connection and removes the virtual display.
    func setEnabled(_ on: Bool) {
        lock.lock(); _enabled = on; lock.unlock()
        if on {
            wake.signal()
        } else {
            dropConnection()
        }
    }

    /// Send video regions as 4:2:2 YCbCr: half the data for video, slightly
    /// softer colour there. Applies immediately; text and still areas stay exact.
    func setSmoothVideo(_ on: Bool) {
        lock.lock(); _smoothVideo = on; lock.unlock()
        encQ.async {
            guard let e = self.enc else { return }
            g4enc_set_yuv(e, on && self.clientYUV ? 1 : 0)
            self.pending = true      // re-encode so video tiles become exact if turned off
            self.trySend()
        }
    }

    /// With Smoother Video: send video regions at half width and height too
    /// (a quarter of the data, softer while playing). Applies immediately;
    /// PowerPC apps older than 0.11 just keep getting full resolution.
    func setFastVideo(_ on: Bool) {
        lock.lock(); _fastVideo = on; lock.unlock()
        encQ.async {
            guard let e = self.enc else { return }
            g4enc_set_yuv_half(e, on && self.clientHalfVideo ? 1 : 0)
        }
    }

    /// Switch between extending (a new virtual display) and mirroring the main
    /// display. Applied by dropping the connection; it reconnects at once.
    func setMirror(_ on: Bool) {
        lock.lock(); let changed = _mirror != on; _mirror = on; lock.unlock()
        guard changed else { return }
        dropConnection()
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

    /// Forget every paired PowerPC Mac (they'll ask to pair again).
    func forgetPairings() {
        PairingStore.forgetAll()
        dropConnection()
    }

    func dropConnection() {
        encQ.async { if self.fd >= 0 { shutdown(self.fd, SHUT_RDWR) } }
    }

    func post(_ s: Status) {
        statusLock.lock()
        let changed = s != lastStatus
        lastStatus = s
        statusLock.unlock()
        guard changed else { return }
        DispatchQueue.main.async { self.onStatus?(s) }
    }

    // MARK: - connection lifecycle (connection thread)

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
                log("bad hello"); endpointSkipUntil = nowMs() + 30_000; close(sock); sleep(1); continue
            }
            guard hello.supportsAuth else {
                log("\(name) runs an older PowerPC Display without pairing")
                post(.failed("Update PowerPC Display on “\(name)”: this version only connects to paired Macs."))
                close(sock); sleep(5); continue
            }
            guard authenticate(sock, hello: hello, name: name) else {
                close(sock); sleep(2); continue
            }

            (W, H, depth, slots, widthMM) = (hello.width, hello.height, hello.depth, hello.slots, hello.widthMM)
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
            let yuv = hello.depth == 32 && hello.supportsYUV
            let half = yuv && hello.supportsHalfVideo
            encQ.sync { beginStreaming(sock, yuv: yuv, halfVideo: half, mirroring: mirroring) }

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
                // don't keep reconnecting (each attempt would blank the
                // PowerPC Mac's screen); wait until permission is granted
                while !CGPreflightScreenCaptureAccess() && isEnabled { sleep(2) }
                sleep(1)
                continue
            }
            post(.connected(name: name, width: W, height: H, depth: depth))
            let did = displayID, sc = hintScale, ox = hintOffX, oy = hintOffY
            DispatchQueue.main.async {
                self.cursor.start(host: self, displayID: did, scale: sc, offsetX: ox, offsetY: oy)
            }

            readerLoop(sock)      // blocks until the PowerPC Mac goes away (or we pause)
            teardown(sock)
            log("\(name) disconnected")
            sleep(1)
        }
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
        encQ.sync { endStreaming(sock) }
    }

    /// Runs async work from the connection thread (not a Swift concurrency
    /// thread, so blocking here can't starve the cooperative pool) and waits.
    /// Returns false on error.
    func runAsync(_ body: @escaping @Sendable () async throws -> Void) -> Bool {
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
}

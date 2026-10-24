import CCodec
import Foundation

/// Encoding, flow control, liveness and the messages to the PowerPC Mac.
/// Everything here runs on encQ unless noted.
extension Host {
    /// New connection: fresh encoder, counters and the ping timer.
    func beginStreaming(_ sock: Int32, yuv: Bool, halfVideo: Bool, mirroring: Bool) {
        fd = sock
        if let e = enc { g4enc_destroy(e) }
        enc = g4enc_create(Int32(W), Int32(H), Int32(depth))
        if let e = enc, slots > 0, g4enc_enable_cache(e, Int32(slots)) != 0 {
            log("tile cache disabled (out of memory)")
        }
        clientYUV = yuv
        clientHalfVideo = halfVideo
        videoLevel = 1; linkRate = 0; calmMs = 0; upWaitMs = 4000; lastUpAt = 0
        applyVideoLevel()
        sentID = 0; ackedID = 0; pending = false
        sendTimes.removeAll()
        peerVersion = ""; offered = nil; sentBrightness = -1
        rttAvg = 0; paced = false
        needFullIngest = true
        lastHeard = nowMs()
        startPings()
    }

    func endStreaming(_ sock: Int32) {
        pingTimer?.cancel()
        pingTimer = nil
        close(sock)
        fd = -1
        if let e = enc { g4enc_destroy(e) }
        enc = nil
    }

    // MARK: liveness

    /// A ping every second; the PowerPC Mac answers, and stops waiting for us
    /// if they stop. If it goes quiet for 4 s (cable pulled, asleep, frozen),
    /// drop the connection so both sides recover instead of hanging.
    private func startPings() {
        pingTimer?.cancel()
        let t = DispatchSource.makeTimerSource(queue: encQ)
        t.schedule(deadline: .now() + 1, repeating: 1, leeway: .milliseconds(100))
        t.setEventHandler { [weak self] in
            guard let self, self.fd >= 0 else { return }
            if nowMs() - self.lastHeard > 4000 {
                log("the PowerPC Mac stopped responding; disconnecting")
                shutdown(self.fd, SHUT_RDWR)
                return
            }
            self.pingSeq &+= 1
            var p = [UInt8](repeating: 0, count: 4)
            put32(&p, 0, self.pingSeq)
            if !writeAll(self.fd, message(type: Int(G4_MSG_PING), payload: p)) { shutdown(self.fd, SHUT_RDWR) }
        }
        t.resume()
        pingTimer = t
    }

    // MARK: sending frames

    /// Encode whatever changed since the PowerPC Mac's last known state — but
    /// only if fewer than `maxInFlight` frames are unacknowledged. Otherwise
    /// wait: the next ACK calls this again and everything that changed
    /// meanwhile goes out as one coalesced frame. This is what keeps lag low.
    func trySend() {
        guard pending, fd >= 0, let enc else { return }
        if sentID &- ackedID >= inFlightLimit() { statBlocked += 1; return }

        var out: UnsafePointer<UInt8>?
        var st = g4_enc_stats()
        g4enc_set_time(enc, UInt32(truncatingIfNeeded: Int64(nowMs())))
        let n = g4enc_encode(enc, sentID &+ 1, &out, &st)
        pending = false
        guard n > 0, let out else { return }

        if writeAll(fd, out, n) {
            scheduleRefreshIfNeeded()
            sentID &+= 1
            sendTimes[sentID] = nowMs()
            statFrames += 1; statBytes += n; statRaw += Int(st.raw_bytes); statCopies += Int(st.copies)
            statHits += Int(st.cache_hits)
            statYuv += Int(st.yuvs); statRefresh += Int(st.refreshes); statPal += Int(st.pals)
            statVideo += Int(st.video_tiles)
        } else {
            log("sending to the PowerPC Mac failed or timed out; disconnecting")
            shutdown(fd, SHUT_RDWR)
        }
        reportStats()
    }

    /// Video tiles were sent as 4:2:2. Once they stop changing the screen
    /// may go quiet (ScreenCaptureKit sends nothing), so come back when the
    /// lossless refresh is due and encode again.
    func scheduleRefreshIfNeeded() {
        guard !refreshScheduled, let e = enc else { return }
        let due = g4enc_refresh_due(e, UInt32(truncatingIfNeeded: Int64(nowMs())))
        guard due >= 0 else { return }
        refreshScheduled = true
        encQ.asyncAfter(deadline: .now() + .milliseconds(Int(due) + 20)) { [weak self] in
            guard let self else { return }
            self.refreshScheduled = false
            guard self.enc != nil, self.fd >= 0 else { return }
            self.pending = true
            self.trySend()
            if !self.pending { self.scheduleRefreshIfNeeded() }
        }
    }

    /// Adaptive pacing. Normally 2 frames in flight, so the network and the
    /// PowerPC Mac's decoding overlap (more frames per second). But when the
    /// link is backed up (frames take > 60 ms to reach the screen) while
    /// you're moving the mouse or scrolling, allow only 1: what you see is
    /// then at most one frame behind your hand. Back to 2 once you stop or
    /// the link clears. encQ.
    func inFlightLimit() -> UInt32 {
        if cfg.inFlightFixed || cfg.maxInFlight < 2 { return cfg.maxInFlight }
        let interactive = nowMs() - lastInput < 600
        if !paced, interactive, rttAvg > 60 {
            paced = true
            if cfg.verbose { log(String(format: "pacing: 1 frame in flight while you interact (%.0f ms to screen)", rttAvg)) }
        } else if paced, !interactive || rttAvg < 25 {
            paced = false
            if cfg.verbose { log("pacing: back to \(cfg.maxInFlight) frames in flight") }
        }
        return paced ? 1 : cfg.maxInFlight
    }

    /// Main thread: the mouse moved, clicked or scrolled.
    func noteInput() {
        encQ.async { self.lastInput = nowMs() }
    }

    func onAck(_ id: UInt32) {
        lastHeard = nowMs()
        guard id > ackedID, id <= sentID else { return }
        if let t = sendTimes[id] {
            let rtt = nowMs() - t
            statRtt += rtt; statRttN += 1
            rttAvg = rttAvg == 0 ? rtt : rttAvg * 0.8 + rtt * 0.2
        }
        for k in sendTimes.keys where k <= id { sendTimes[k] = nil }
        ackedID = id
        trySend()
    }

    // MARK: adaptive video quality

    /// The level actually used: what the settings and the PowerPC app allow.
    var effectiveVideoLevel: Int {
        guard clientYUV, smoothVideo else { return 0 }
        guard fastVideo else { return 1 }
        return min(videoLevel, clientHalfVideo ? 2 : 1)
    }

    func applyVideoLevel() {
        guard let e = enc else { return }
        let l = effectiveVideoLevel
        g4enc_set_yuv(e, l >= 1 ? 1 : 0)
        g4enc_set_yuv_half(e, l >= 2 ? 1 : 0)
    }

    /// Every stats window (~2 s) while video plays: the best quality the
    /// link carries. Lossless → 4:2:2 → half resolution as soon as frames
    /// back up (the PowerPC Mac or the network can't keep up); back up a
    /// step after a calm while, if the measured link rate leaves room for
    /// the bigger frames. A step up that didn't fit makes the next try wait
    /// longer (up to a minute), so it doesn't keep flickering between two.
    func adaptVideo(windowMs: Double) {
        guard fastVideo, smoothVideo, clientYUV, statVideo > 0 else { calmMs = 0; return }
        let worst = clientHalfVideo ? 2 : 1
        let tries = statFrames + statBlocked
        let rate = Double(statBytes) / (windowMs / 1000)
        let full = tries > 0 && (Double(statBlocked) / Double(tries) > 0.25 || rttAvg > 90)
        let old = videoLevel
        if full {
            linkRate = linkRate == 0 ? rate : linkRate * 0.7 + rate * 0.3
            calmMs = 0
            if videoLevel < worst {
                videoLevel += 1
                if nowMs() - lastUpAt < 10_000 { upWaitMs = min(upWaitMs * 2, 64_000) }   // that step up didn't fit
            }
        } else {
            calmMs += windowMs
            if videoLevel > 0, calmMs >= upWaitMs {
                // half → full resolution is ~4x the video data, 4:2:2 → lossless ~2x
                let growth = videoLevel == 2 ? 3.0 : 2.0
                if linkRate == 0 || rate * growth < linkRate * 0.8 {
                    videoLevel -= 1
                    lastUpAt = nowMs()
                }
                calmMs = 0
            }
        }
        if videoLevel != old {
            applyVideoLevel()
            if cfg.verbose {
                log(String(format: "video quality: %@ (%.0f KB/s, link carries ~%.0f KB/s)",
                           ["sharp (lossless)", "smooth (4:2:2)", "half resolution"][effectiveVideoLevel] as NSString,
                           rate / 1024, linkRate / 1024))
            }
        }
    }

    func reportStats() {
        let dt = nowMs() - statT0
        guard dt > 2000 else { return }
        let s = dt / 1000
        var snap = Stats()
        snap.fps = Double(statFrames) / s
        snap.kbps = Double(statBytes) / 1024 / s
        snap.compression = statBytes > 0 ? Double(statRaw) / Double(statBytes) : 0
        snap.latencyMs = statRttN > 0 ? statRtt / Double(statRttN) : 0
        snap.at = nowMs()
        adaptVideo(windowMs: dt)
        if statVideo > 0 { snap.videoQuality = ["sharp", "smooth", "half"][effectiveVideoLevel] }
        DispatchQueue.main.async { self.onStats?(snap) }
        if cfg.verbose {
            log(String(format: "%.1f fps  %.0f KB/s  (%.1fx)  frame→screen %.1f ms  waits %d  scroll copies %d  cache hits %d  palette rects %d  video rects %d  sharpened tiles %d  video %@",
                       snap.fps, snap.kbps, snap.compression, snap.latencyMs, statBlocked, statCopies, statHits, statPal, statYuv, statRefresh,
                       (snap.videoQuality ?? "none") as NSString))
        }
        statT0 = nowMs(); statFrames = 0; statBytes = 0; statRaw = 0; statBlocked = 0
        statCopies = 0; statHits = 0; statRtt = 0; statRttN = 0; statYuv = 0; statRefresh = 0; statPal = 0; statVideo = 0
    }

    // MARK: reading (connection thread)

    /// ACKs and PONGs from the PowerPC Mac, until the connection ends.
    func readerLoop(_ sock: Int32) {
        var hdr = [UInt8](repeating: 0, count: 8)
        var payload = [UInt8](repeating: 0, count: 64)
        while readAll(sock, &hdr, 8) {
            let len = Int(be32(hdr, 4))
            if len > 1 << 20 { break }
            if len > payload.count { payload = [UInt8](repeating: 0, count: len) }
            if len > 0, !readAll(sock, &payload, len) { break }
            let type = Int(hdr[0])
            if type == Int(G4_MSG_ACK), len >= 4 {
                let id = be32(payload, 0)
                encQ.async { self.onAck(id) }
            } else if type == Int(G4_MSG_PONG) {
                encQ.async { self.lastHeard = nowMs() }
            } else if type == Int(G4_MSG_INFO), len > 0, len <= 32 {
                let v = String(decoding: payload[0..<len], as: UTF8.self)
                encQ.async { self.onClientVersion(v) }
            } else if type == Int(G4_MSG_UPDATE_REQ) {
                encQ.async { self.sendUpdate() }
            }
        }
    }

    // MARK: cursor channel (called from the main thread)

    func sendCursorPos(x: Int, y: Int, visible: Bool) {
        var p = [UInt8](repeating: 0, count: 8)
        put16(&p, 0, x); put16(&p, 2, y); p[4] = visible ? 1 : 0
        let m = message(type: Int(G4_MSG_CURSOR_POS), payload: p)
        encQ.async {
            guard self.fd >= 0 else { return }
            if !writeAll(self.fd, m) { shutdown(self.fd, SHUT_RDWR) }
        }
    }

    func sendCursorShape(width: Int, height: Int, hotX: Int, hotY: Int, argb: [UInt8]) {
        var p = [UInt8](repeating: 0, count: 8)
        put16(&p, 0, width); put16(&p, 2, height); put16(&p, 4, hotX); put16(&p, 6, hotY)
        let m = message(type: Int(G4_MSG_CURSOR_SHAPE), payload: p + argb)
        encQ.async {
            guard self.fd >= 0 else { return }
            if !writeAll(self.fd, m) { shutdown(self.fd, SHUT_RDWR) }
        }
    }
}

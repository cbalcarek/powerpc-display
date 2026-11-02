import CCodec
import CoreAudio
import CoreMedia
import Foundation
import ScreenCaptureKit

// This Mac's sound, played on the PowerPC Mac. ScreenCaptureKit captures it
// along with the screen (48 kHz stereo); every 20 ms of it goes out as IMA
// ADPCM (4 bits a sample, about 48 KB/s), which even a G3 decodes almost for
// free. One PowerPC Mac at a time plays it (see HostGroup.setSound).

extension Host {
    static let audioRate = 48_000
    static let audioPacketFrames = 960            // 20 ms

    var soundOn: Bool { lock.lock(); defer { lock.unlock() }; return _sound }

    func setSound(_ on: Bool) {
        lock.lock(); _sound = on; lock.unlock()
        encQ.async { self.refreshAudioOn() }
    }

    /// encQ: start or stop capturing sound for it (setting changed, or a new connection).
    func refreshAudioOn() {
        let want = fd >= 0 && clientAudio && soundOn
        guard want != audioOn else { return }
        lock.lock(); let s = _stream, conf = _scConfig; lock.unlock()
        guard let s, let conf else { return }
        audioOn = want
        conf.capturesAudio = want
        Task { try? await s.updateConfiguration(conf) }
        log(want ? "playing this Mac's sound on “\(peerName)”" : "stopped playing sound on “\(peerName)”")
    }

    /// Sound settings for a new capture (encQ state read by the caller).
    func configureAudio(_ sc: SCStreamConfiguration, on: Bool) {
        sc.capturesAudio = on
        sc.sampleRate = Self.audioRate
        sc.channelCount = 2
        sc.excludesCurrentProcessAudio = true
    }

    /// audioQ: a buffer of captured sound -> 20 ms ADPCM packets.
    func handleAudio(_ sb: CMSampleBuffer) {
        guard let f = sb.formatDescription?.audioStreamBasicDescription,
              f.mFormatID == kAudioFormatLinearPCM, f.mBitsPerChannel == 32,
              f.mFormatFlags & kAudioFormatFlagIsFloat != 0, f.mChannelsPerFrame >= 1 else { return }
        let frames = CMSampleBufferGetNumSamples(sb)
        guard frames > 0 else { return }
        let ch = Int(f.mChannelsPerFrame)
        let planar = f.mFormatFlags & kAudioFormatFlagIsNonInterleaved != 0
        @inline(__always) func s16(_ v: Float) -> Int16 { Int16(max(-1, min(1, v)) * 32767) }
        try? sb.withAudioBufferList { abl, _ in
            guard let d0 = abl.first?.mData else { return }
            var out = [Int16](repeating: 0, count: frames * 2)
            if planar {
                let l = d0.assumingMemoryBound(to: Float.self)
                let r = (abl.count > 1 ? abl[1].mData : nil)?.assumingMemoryBound(to: Float.self) ?? l
                for i in 0..<frames { out[2 * i] = s16(l[i]); out[2 * i + 1] = s16(r[i]) }
            } else {
                let p = d0.assumingMemoryBound(to: Float.self)
                let right = ch > 1 ? 1 : 0
                for i in 0..<frames { out[2 * i] = s16(p[i * ch]); out[2 * i + 1] = s16(p[i * ch + right]) }
            }
            audioPending.append(contentsOf: out)
        }
        let rate = UInt32(f.mSampleRate.rounded())
        let n = Self.audioPacketFrames
        while audioPending.count >= n * 2 {
            let chunk = Array(audioPending[0..<(n * 2)])
            audioPending.removeFirst(n * 2)
            var pkt = [UInt8](repeating: 0, count: Int(G4_AUDIO_HDR) + 2 * (4 + n / 2))
            audioSeq &+= 1
            put32(&pkt, 0, audioSeq)
            put32(&pkt, 4, rate)
            put16(&pkt, 8, n)
            pkt[10] = 2
            pkt[11] = UInt8(G4_AUDIO_ADPCM)
            chunk.withUnsafeBufferPointer { c in
                pkt.withUnsafeMutableBufferPointer { o in
                    adpcm.withUnsafeMutableBufferPointer { st in
                        _ = g4_adpcm_encode(st.baseAddress, c.baseAddress, UInt32(n), 2, o.baseAddress! + Int(G4_AUDIO_HDR))
                    }
                }
            }
            let m = message(type: Int(G4_MSG_AUDIO), payload: pkt)
            encQ.async {
                guard self.fd >= 0, self.audioOn else { return }
                if !writeAll(self.fd, m) { shutdown(self.fd, SHUT_RDWR) }
            }
        }
    }

    /// audioQ state for a new connection.
    func resetAudio() {
        audioQ.async {
            self.adpcm = [g4_adpcm_state](repeating: g4_adpcm_state(pred: 0, index: 0), count: 2)
            self.audioPending.removeAll()
            self.audioSeq = 0
        }
    }
}

import CCodec
import CDiscovery
import Foundation

/// Sleeping and waking with this Mac. When it (or its screens) goes to sleep
/// the PowerPC Mac is told to sleep too; on wake it's woken with
/// Wake-on-LAN and the connection comes back as usual.
extension Host {
    private var macKey: String { cfg.g4Name.map { "peerMAC.\($0)" } ?? "peerMAC" }

    var isAsleep: Bool { lock.lock(); defer { lock.unlock() }; return _asleep }

    func rememberMAC(_ mac: [UInt8]) {
        UserDefaults.standard.set(Data(mac), forKey: macKey)
    }

    var rememberedMAC: [UInt8]? {
        guard let d = UserDefaults.standard.data(forKey: macKey), d.count == 6 else { return nil }
        return [UInt8](d)
    }

    /// Main thread: this Mac or its displays are going to sleep.
    /// releaseDisplay: also remove the virtual display while asleep (lid
    /// closed: otherwise macOS treats it as an external monitor and keeps a
    /// laptop awake in clamshell mode instead of sleeping).
    func macWillSleep(_ why: String, releaseDisplay: Bool = false) {
        lock.lock()
        let already = _asleep
        _asleep = true
        if releaseDisplay { _releaseWhileAsleep = true }
        lock.unlock()
        if releaseDisplay { wake.signal() }            // the connection thread removes it
        guard !already else { return }
        // never hold up the main thread for long (a stuck connection must
        // not freeze the app while the Mac goes to sleep)
        var told = false
        let done = DispatchSemaphore(value: 0)
        encQ.async {
            if self.fd >= 0 {
                if self.clientSleeps, writeAll(self.fd, message(type: Int(G4_MSG_SLEEP), payload: [])) { told = true }
                shutdown(self.fd, SHUT_RDWR)
            }
            done.signal()
        }
        if done.wait(timeout: .now() + 1.5) == .timedOut {
            log("\(why): the connection didn't respond; dropping it")
            dropConnection()
        }
        if told {
            lock.lock(); _peerSleeping = true; lock.unlock()
            log("\(why): told the PowerPC Mac to sleep")
        } else {
            log("\(why): pausing until it wakes")
        }
    }

    /// Main thread: awake again. Wake the PowerPC Mac if we put it to sleep,
    /// and reconnect.
    func macDidWake(_ why: String) {
        lock.lock()
        let was = _asleep, wakePeer = _peerSleeping
        _asleep = false; _peerSleeping = false; _releaseWhileAsleep = false
        lock.unlock()
        guard was else { return }
        wake.signal()
        guard wakePeer, let mac = rememberedMAC else { log("\(why): reconnecting"); return }
        log("\(why): waking the PowerPC Mac (Wake-on-LAN)")
        DispatchQueue.global(qos: .utility).async {
            // keep knocking until it answers (it may take a while to wake)
            for _ in 0..<20 {
                if self.isAsleep || self.encQ.sync(execute: { self.fd >= 0 }) { return }
                _ = g4_wake_on_lan(mac)
                Thread.sleep(forTimeInterval: 2)
            }
        }
    }
}

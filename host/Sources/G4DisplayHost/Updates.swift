import AppKit
import CCodec
import Foundation

/// Versions like "0.12": compare the dot-separated numbers.
func versionNewer(_ a: String, than b: String) -> Bool {
    let x = a.split(separator: ".").map { Int($0.filter(\.isNumber)) ?? 0 }
    let y = b.split(separator: ".").map { Int($0.filter(\.isNumber)) ?? 0 }
    for i in 0..<max(x.count, y.count) {
        let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
        if p != q { return p > q }
    }
    return false
}

/// The newest PowerPC (Tiger) app this Mac has, to hand to PowerPC Macs
/// running an older one: downloaded from GitHub, the project's releases
/// folder (where the Tiger build sends it), or built into this app.
enum TigerUpdate {
    struct Package { let version: String; let data: Data; let sha512: [UInt8] }

    static var supportDir: URL {
        let d = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("PowerPC Display", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }
    static var downloadedZip: URL { supportDir.appendingPathComponent("PowerPC-Display-PPC.zip") }
    static var bundledZip: URL? { Bundle.main.url(forResource: "PowerPC-Display-PPC", withExtension: "zip") }
    /// releases/ in the project folder (set by Build Mac App.command)
    static var releasesZip: URL? {
        guard let d = UserDefaults.standard.string(forKey: "releasesDir") else { return nil }
        return URL(fileURLWithPath: d).appendingPathComponent("PowerPC-Display-PPC.zip")
    }

    private static let lock = NSLock()
    private static var cached: (key: String, pkg: Package?)?

    /// The version inside a Tiger zip (its Info.plist), without unpacking it all.
    static func version(of zip: URL) -> String? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/unzip")
        p.arguments = ["-p", zip.path, "PowerPC Display.app/Contents/Info.plist"]
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        guard let plist = try? PropertyListSerialization.propertyList(from: data, format: nil) as? [String: Any],
              plist["CFBundleIdentifier"] as? String == "local.ppcdisplay.client" else { return nil }
        return plist["CFBundleShortVersionString"] as? String
    }

    /// Newest of the downloaded and built-in copies (cached until a file changes).
    static func latest() -> Package? {
        let candidates = [downloadedZip, releasesZip, bundledZip].compactMap { $0 }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
        let key = candidates.map { u -> String in
            let d = (try? FileManager.default.attributesOfItem(atPath: u.path))?[.modificationDate] as? Date
            return u.path + "@" + String(d?.timeIntervalSince1970 ?? 0)
        }.joined(separator: "|")
        lock.lock(); defer { lock.unlock() }
        if let c = cached, c.key == key { return c.pkg }
        var best: (String, URL)?
        for u in candidates {
            guard let v = version(of: u) else { continue }
            if best == nil || versionNewer(v, than: best!.0) { best = (v, u) }
        }
        var pkg: Package?
        if let b = best, let data = try? Data(contentsOf: b.1) {
            let v = b.0
            var h = [UInt8](repeating: 0, count: 64)
            data.withUnsafeBytes { raw in
                g4_sha512(&h, raw.bindMemory(to: UInt8.self).baseAddress, data.count)
            }
            pkg = Package(version: v, data: data, sha512: h)
        }
        cached = (key, pkg)
        return pkg
    }
}

extension Host {
    /// encQ: the PowerPC Mac told us its app version; offer ours if newer.
    func onClientVersion(_ v: String) {
        peerVersion = v
        DispatchQueue.global(qos: .utility).async {
            guard let pkg = TigerUpdate.latest(), versionNewer(pkg.version, than: v) else { return }
            self.encQ.async {
                guard self.fd >= 0, self.peerVersion == v else { return }
                var p = [UInt8](repeating: 0, count: 4)
                put32(&p, 0, UInt32(pkg.data.count))
                p += pkg.sha512 + Array(pkg.version.utf8)
                log("offering PowerPC Display \(pkg.version) to the PowerPC Mac (it has \(v))")
                self.offered = pkg
                if !writeAll(self.fd, message(type: Int(G4_MSG_UPDATE_OFFER), payload: p)) { shutdown(self.fd, SHUT_RDWR) }
            }
        }
    }

    /// encQ: it asked for the update. ~250 KB: sent in one go between frames.
    func sendUpdate() {
        guard let pkg = offered, fd >= 0 else { return }
        let bytes = [UInt8](pkg.data)
        var off = 0
        while off < bytes.count {
            let n = min(65536, bytes.count - off)
            var p = [UInt8](repeating: 0, count: 4)
            put32(&p, 0, UInt32(off))
            p += bytes[off..<(off + n)]
            if !writeAll(fd, message(type: Int(G4_MSG_UPDATE_DATA), payload: p)) { shutdown(fd, SHUT_RDWR); return }
            off += n
        }
        log("sent PowerPC Display \(pkg.version) to the PowerPC Mac")
    }
}

import CCodec
import CoreGraphics
import Foundation

/// This Mac's built-in screen brightness (0...1), read with macOS's
/// DisplayServices (not public, but what Apple Silicon Macs use; if a future
/// macOS changes it, brightness matching just stops).
enum Brightness {
    private typealias GetFn = @convention(c) (CGDirectDisplayID, UnsafeMutablePointer<Float>) -> Int32
    private static let get: GetFn? = {
        guard let h = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_LAZY),
              let f = dlsym(h, "DisplayServicesGetBrightness") else { return nil }
        return unsafeBitCast(f, to: GetFn.self)
    }()

    /// The built-in display (an iMac's or a MacBook's), else the main one.
    private static var display: CGDirectDisplayID {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var n: UInt32 = 0
        CGGetOnlineDisplayList(16, &ids, &n)
        return ids.prefix(Int(n)).first { CGDisplayIsBuiltin($0) != 0 } ?? CGMainDisplayID()
    }

    static func current() -> Float? {
        guard let get else { return nil }
        var v: Float = -1
        guard get(display, &v) == 0, v >= 0, v <= 1 else { return nil }
        return v
    }
}

extension Host {
    /// encQ: match this brightness (0...1000, or 0xFFFF: the PowerPC Mac's own again).
    func sendBrightness(_ level: Int) {
        guard fd >= 0, level != sentBrightness else { return }
        if level != 0xFFFF, sentBrightness >= 0, sentBrightness != 0xFFFF, abs(level - sentBrightness) < 5 { return }
        var p = [UInt8](repeating: 0, count: 2)
        put16(&p, 0, level)
        if writeAll(fd, message(type: Int(G4_MSG_BRIGHTNESS), payload: p)) { sentBrightness = level }
    }
}

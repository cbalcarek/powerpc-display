import CoreGraphics
import Foundation
import IOKit

/// A laptop's lid, from the power manager (nil on Macs without one).
enum Lid {
    static func isClosed() -> Bool? {
        let root = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOPMrootDomain"))
        guard root != 0 else { return nil }
        defer { IOObjectRelease(root) }
        guard let v = IORegistryEntryCreateCFProperty(root, "AppleClamshellState" as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() else { return nil }
        return (v as? Bool) ?? (v as? NSNumber)?.boolValue
    }

    /// Is a real external monitor attached (not built in, not one of ours)?
    static func realExternalDisplay(excluding ours: Set<CGDirectDisplayID>) -> Bool {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var n: UInt32 = 0
        CGGetOnlineDisplayList(16, &ids, &n)
        return ids.prefix(Int(n)).contains { CGDisplayIsBuiltin($0) == 0 && !ours.contains($0) }
    }
}

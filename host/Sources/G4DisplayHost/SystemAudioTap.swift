import CoreAudio
import Foundation

/// Everything this Mac plays, captured with a Core Audio process tap (macOS
/// 14.2+) and muted here while it's being captured, so it plays only on the
/// PowerPC Mac. macOS asks once for "System Audio Recording" permission
/// (NSAudioCaptureUsageDescription). Not thread-safe: start/stop on one queue.
@available(macOS 14.2, *)
final class SystemAudioTap {
    private var tapID = AudioObjectID(kAudioObjectUnknown)
    private var aggregateID = AudioObjectID(kAudioObjectUnknown)
    private var procID: AudioDeviceIOProcID?
    private let queue: DispatchQueue
    private let onAudio: (UnsafeMutableAudioBufferListPointer, AudioStreamBasicDescription) -> Void

    /// onAudio runs on `queue` with each buffer of float samples.
    init(queue: DispatchQueue,
         onAudio: @escaping (UnsafeMutableAudioBufferListPointer, AudioStreamBasicDescription) -> Void) {
        self.queue = queue
        self.onAudio = onAudio
    }

    deinit { stop() }

    /// false: couldn't set it up (the caller falls back to ScreenCaptureKit).
    func start() -> Bool {
        // every process, mixed to stereo, silenced on this Mac while tapped
        let desc = CATapDescription(stereoGlobalTapButExcludeProcesses: [])
        desc.name = "PowerPC Display"
        desc.isPrivate = true
        desc.muteBehavior = .mutedWhenTapped
        guard AudioHardwareCreateProcessTap(desc, &tapID) == noErr, tapID != kAudioObjectUnknown else {
            log("audio tap: couldn't create it")
            return false
        }
        var format = AudioStreamBasicDescription()
        var addr = AudioObjectPropertyAddress(mSelector: kAudioTapPropertyFormat,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        guard AudioObjectGetPropertyData(tapID, &addr, 0, nil, &size, &format) == noErr,
              let outputUID = Self.defaultOutputUID() else {
            log("audio tap: no format or output device")
            stop()
            return false
        }

        // a private aggregate device with the tap as its input, clocked by
        // the output device
        let device: [String: Any] = [
            kAudioAggregateDeviceNameKey: "PowerPC Display sound",
            kAudioAggregateDeviceUIDKey: "local.ppcdisplay.tap.\(UUID().uuidString)",
            kAudioAggregateDeviceMainSubDeviceKey: outputUID,
            kAudioAggregateDeviceIsPrivateKey: true,
            kAudioAggregateDeviceIsStackedKey: false,
            kAudioAggregateDeviceTapAutoStartKey: true,
            kAudioAggregateDeviceSubDeviceListKey: [[kAudioSubDeviceUIDKey: outputUID]],
            kAudioAggregateDeviceTapListKey: [[kAudioSubTapDriftCompensationKey: true,
                                               kAudioSubTapUIDKey: desc.uuid.uuidString]],
        ]
        guard AudioHardwareCreateAggregateDevice(device as CFDictionary, &aggregateID) == noErr else {
            log("audio tap: couldn't create the aggregate device")
            stop()
            return false
        }

        let f = format
        let err = AudioDeviceCreateIOProcIDWithBlock(&procID, aggregateID, queue) { [weak self] _, input, _, _, _ in
            self?.onAudio(UnsafeMutableAudioBufferListPointer(UnsafeMutablePointer(mutating: input)), f)
        }
        guard err == noErr, let procID else {
            log("audio tap: couldn't add an IO proc")
            stop()
            return false
        }
        // a new aggregate device can take a moment to be ready
        for attempt in 0..<5 {
            if AudioDeviceStart(aggregateID, procID) == noErr {
                log("audio tap: \(Int(f.mSampleRate)) Hz, \(f.mChannelsPerFrame) channels")
                return true
            }
            if attempt < 4 { Thread.sleep(forTimeInterval: 0.1) }
        }
        log("audio tap: couldn't start")
        stop()
        return false
    }

    func stop() {
        if let p = procID {
            AudioDeviceStop(aggregateID, p)
            AudioDeviceDestroyIOProcID(aggregateID, p)
            procID = nil
        }
        if aggregateID != kAudioObjectUnknown {
            AudioHardwareDestroyAggregateDevice(aggregateID)
            aggregateID = AudioObjectID(kAudioObjectUnknown)
        }
        if tapID != kAudioObjectUnknown {
            AudioHardwareDestroyProcessTap(tapID)
            tapID = AudioObjectID(kAudioObjectUnknown)
        }
    }

    /// The UID of the current default output device.
    private static func defaultOutputUID() -> String? {
        var dev = AudioObjectID(kAudioObjectUnknown)
        var size = UInt32(MemoryLayout<AudioObjectID>.size)
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDefaultOutputDevice,
                                              mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &dev) == noErr,
              dev != kAudioObjectUnknown else { return nil }
        var uid: Unmanaged<CFString>?
        size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        addr.mSelector = kAudioDevicePropertyDeviceUID
        guard AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &uid) == noErr,
              let u = uid?.takeRetainedValue() else { return nil }
        return u as String
    }
}

import Darwin
import Foundation
import os

private let logger = Logger(subsystem: "local.ppcdisplay.host", category: "app")

/// Prints to the Terminal (when run from there) and to the unified log, so
/// problems can be read later in Console.app (search "PowerPC Display" or
/// subsystem local.ppcdisplay.host) without having started it from Terminal.
func log(_ s: String) {
    let ts = String(format: "%.3f", Date().timeIntervalSince1970.truncatingRemainder(dividingBy: 1000))
    print("[\(ts)] \(s)")
    logger.notice("\(s, privacy: .public)")
}

/// Monotonic milliseconds.
func nowMs() -> Double {
    Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1_000_000
}

/// `ifindex`, when non-zero, pins the connection to one network interface
/// (needed for self-assigned 169.254.x.x addresses on a direct cable, which
/// otherwise might be routed out over Wi-Fi).
func connectTCP(host: String, port: UInt16, ifindex: UInt32 = 0, timeoutMs: Int32 = 3000) -> Int32? {
    var hints = addrinfo()
    hints.ai_family = AF_INET
    hints.ai_socktype = SOCK_STREAM
    hints.ai_protocol = IPPROTO_TCP
    var res: UnsafeMutablePointer<addrinfo>?
    guard getaddrinfo(host, String(port), &hints, &res) == 0, let ai = res else { return nil }
    defer { freeaddrinfo(res) }

    let fd = socket(ai.pointee.ai_family, ai.pointee.ai_socktype, ai.pointee.ai_protocol)
    if fd < 0 { return nil }
    var one: Int32 = 1
    setsockopt(fd, Int32(IPPROTO_TCP), TCP_NODELAY, &one, socklen_t(MemoryLayout<Int32>.size))
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
    if ifindex != 0 {
        var idx = ifindex
        let IP_BOUND_IF_: Int32 = 25   // <netinet/in.h> IP_BOUND_IF
        setsockopt(fd, Int32(IPPROTO_IP), IP_BOUND_IF_, &idx, socklen_t(MemoryLayout<UInt32>.size))
    }
    // keep the kernel send queue modest: with ack-based flow control we never
    // want megabytes of stale screen sitting in a buffer
    var sndbuf: Int32 = 512 * 1024
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, socklen_t(MemoryLayout<Int32>.size))
    // a write that can't make progress for 3 s fails instead of blocking the
    // send queue forever (cable pulled, PowerPC Mac frozen)
    var sndto = timeval(tv_sec: 3, tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, socklen_t(MemoryLayout<timeval>.size))

    // connect with a timeout: a silently dropped connection would otherwise
    // block for over a minute while the UI keeps saying "Looking…"
    let flags = fcntl(fd, F_GETFL, 0)
    _ = fcntl(fd, F_SETFL, flags | O_NONBLOCK)
    if connect(fd, ai.pointee.ai_addr, ai.pointee.ai_addrlen) != 0 {
        guard errno == EINPROGRESS else { close(fd); return nil }
        var pfd = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
        guard poll(&pfd, 1, timeoutMs) == 1 else { close(fd); return nil }
        var soErr: Int32 = 0
        var len = socklen_t(MemoryLayout<Int32>.size)
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len)
        guard soErr == 0 else { close(fd); return nil }
    }
    _ = fcntl(fd, F_SETFL, flags)
    return fd
}

/// false on error, including the 3 s send timeout (EAGAIN)
func writeAll(_ fd: Int32, _ p: UnsafeRawPointer, _ n: Int) -> Bool {
    var off = 0
    while off < n {
        let r = Darwin.write(fd, p + off, n - off)
        if r < 0 {
            if errno == EINTR { continue }
            return false
        }
        off += r
    }
    return true
}

func writeAll(_ fd: Int32, _ bytes: [UInt8]) -> Bool {
    bytes.withUnsafeBytes { writeAll(fd, $0.baseAddress!, $0.count) }
}

func readAll(_ fd: Int32, _ p: UnsafeMutableRawPointer, _ n: Int) -> Bool {
    var off = 0
    while off < n {
        let r = Darwin.read(fd, p + off, n - off)
        if r == 0 { return false }
        if r < 0 {
            if errno == EINTR { continue }
            return false
        }
        off += r
    }
    return true
}

/// Reads one complete message, waiting at most `timeoutMs` for it to start.
/// Returns (type, payload), or nil on timeout, error or an oversized message.
func readMessage(_ fd: Int32, timeoutMs: Int32, maxLen: Int = 4096) -> (Int, [UInt8])? {
    var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
    guard poll(&pfd, 1, timeoutMs) == 1 else { return nil }
    var hdr = [UInt8](repeating: 0, count: 8)
    guard readAll(fd, &hdr, 8) else { return nil }
    let len = Int(be32(hdr, 4))
    guard len <= maxLen else { return nil }
    var payload = [UInt8](repeating: 0, count: len)
    if len > 0, !readAll(fd, &payload, len) { return nil }
    return (Int(hdr[0]), payload)
}

@inline(__always) func be16(_ b: [UInt8], _ i: Int) -> Int { Int(b[i]) << 8 | Int(b[i + 1]) }
@inline(__always) func be32(_ b: [UInt8], _ i: Int) -> UInt32 {
    UInt32(b[i]) << 24 | UInt32(b[i + 1]) << 16 | UInt32(b[i + 2]) << 8 | UInt32(b[i + 3])
}
@inline(__always) func put16(_ b: inout [UInt8], _ i: Int, _ v: Int) {
    b[i] = UInt8(truncatingIfNeeded: v >> 8); b[i + 1] = UInt8(truncatingIfNeeded: v)
}
@inline(__always) func put32(_ b: inout [UInt8], _ i: Int, _ v: UInt32) {
    b[i] = UInt8(truncatingIfNeeded: v >> 24); b[i + 1] = UInt8(truncatingIfNeeded: v >> 16)
    b[i + 2] = UInt8(truncatingIfNeeded: v >> 8); b[i + 3] = UInt8(truncatingIfNeeded: v)
}

/// Builds a complete message (8-byte header + payload).
func message(type: Int, payload: [UInt8]) -> [UInt8] {
    var m = [UInt8](repeating: 0, count: 8)
    m[0] = UInt8(type)
    put32(&m, 4, UInt32(payload.count))
    return m + payload
}

/// Reads a fixed-size C char array (imported by Swift as a tuple) as a String.
func cString<T>(_ tuple: inout T) -> String {
    withUnsafeBytes(of: &tuple) { raw in
        String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
    }
}

func hexString(_ bytes: [UInt8]) -> String {
    bytes.map { String(format: "%02x", $0) }.joined()
}

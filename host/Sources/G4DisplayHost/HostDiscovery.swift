import CCodec
import CDiscovery
import Foundation

/// Finding the PowerPC Mac (connection thread).
extension Host {
    private static let endpointKey = "lastEndpoint"

    /// Remember where the PowerPC Mac was reached, across launches too, so a
    /// stuck Bonjour on either Mac doesn't stop it reconnecting.
    func remember(_ host: String, _ port: UInt16, _ name: String) {
        lastEndpoint = (host, port, name)
        UserDefaults.standard.set(["host": host, "port": Int(port), "name": name], forKey: Self.endpointKey)
    }

    func loadRememberedEndpoint() {
        guard cfg.g4Address == nil,
              let d = UserDefaults.standard.dictionary(forKey: Self.endpointKey),
              let host = d["host"] as? String, let port = d["port"] as? Int, let name = d["name"] as? String,
              port > 0, port < 65536
        else { return }
        if let wanted = cfg.g4Name, wanted != name { return }
        lastEndpoint = (host, UInt16(port), name)
    }

    /// Name shown in System Settings › Displays: the PowerPC Mac's computer name.
    var displayLabel: String {
        let n = peerName
        if n.isEmpty || n.first?.isNumber == true || n.contains(":") || n == "localhost" { return "PowerPC Display" }
        return n
    }

    /// Connect by address, or find the PowerPC Mac: its last address first,
    /// then Bonjour. Who is on the other end is checked by the pairing
    /// handshake afterwards, so connecting to the wrong machine is harmless.
    func connectToG4(announced: inout Bool) -> (Int32, String)? {
        if let addr = cfg.g4Address {
            if let s = connectTCP(host: addr, port: cfg.port) { return (s, addr) }
            if !announced { log("waiting for the PowerPC Mac at \(addr):\(cfg.port)…"); announced = true }
            post(.searching(addr))
            return nil
        }
        // the address that worked last time (this run or a previous one)
        // first: no Bonjour needed while the PowerPC Mac's address is the same
        if let ep = lastEndpoint, nowMs() >= endpointSkipUntil {
            if let s = connectTCP(host: ep.host, port: ep.port, timeoutMs: 700) {
                log("connected to \"\(ep.name)\" at its last address \(ep.host):\(ep.port)")
                lastDiscoveryNote = ""
                return (s, ep.name)
            }
            // not there (yet): keep it, and also ask Bonjour below
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
                remember("127.0.0.1", UInt16(G4_PORT), name)
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
                     "Found “\(name)” but couldn't get its details (Bonjour error \(found.dns_error)). If this persists, run “sudo killall mDNSResponder” on the PowerPC Mac or click Restart Bonjour there.")
            default:
                // address lookup failed: fall back to the system resolver on the host name
                if !host.isEmpty, let s = connectTCP(host: host, port: found.port) {
                    log("found \"\(name)\" at \(host):\(found.port) (system resolver)")
                    lastDiscoveryNote = ""
                    remember(host, found.port, name)
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
        remember(via, found.port, name)
        return (s, name)
    }

    /// Log a discovery problem once, and show it in the window.
    func note(_ logLine: String, _ userText: String) {
        if logLine != lastDiscoveryNote {
            log(logLine)
            lastDiscoveryNote = logLine
        }
        post(.cantReach(userText))
    }
}

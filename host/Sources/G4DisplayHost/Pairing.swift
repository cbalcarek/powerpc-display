import AppKit
import CCodec
import Foundation
import Security

/// The PowerPC Mac's HELLO (protocol v2 adds its ID, a nonce and a public key).
struct Hello {
    var version = 0, width = 0, height = 0, depth = 32, slots = 0, widthMM = 0, flags = 0
    var clientID: [UInt8] = []          // 16 bytes, stable per PowerPC Mac
    var nonce: [UInt8] = []             // 16 bytes, fresh per connection
    var publicKey: [UInt8] = []         // 32 bytes, used only when pairing

    var supportsAuth: Bool { flags & Int(G4_HELLO_FLAG_AUTH) != 0 && clientID.count == 16 }
    var supportsYUV: Bool { flags & Int(G4_HELLO_FLAG_YUV) != 0 }
    var supportsHalfVideo: Bool { flags & Int(G4_HELLO_FLAG_YUVH) != 0 }
}

/// Paired PowerPC Macs. Keys live in the login keychain; names (for display
/// only) and this Mac's own ID in UserDefaults.
enum PairingStore {
    private static let service = "local.ppcdisplay.pairing"
    private static let hostIDKey = "hostID"
    private static let namesKey = "pairedNames"

    /// This Mac's installation ID (not secret).
    static var hostID: [UInt8] {
        if let d = UserDefaults.standard.data(forKey: hostIDKey), d.count == Int(G4_ID_BYTES) { return [UInt8](d) }
        var id = [UInt8](repeating: 0, count: Int(G4_ID_BYTES))
        _ = g4_random(&id, Int(G4_ID_BYTES))
        UserDefaults.standard.set(Data(id), forKey: hostIDKey)
        return id
    }

    static func key(for clientID: [UInt8]) -> [UInt8]? {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: hexString(clientID),
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let d = out as? Data, d.count == Int(G4_KEY_BYTES) else { return nil }
        return [UInt8](d)
    }

    static func save(key: [UInt8], for clientID: [UInt8], name: String) {
        forget(clientID)
        let item: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: hexString(clientID),
            kSecAttrLabel as String: "PowerPC Display pairing (\(name))",
            kSecValueData as String: Data(key),
        ]
        let st = SecItemAdd(item as CFDictionary, nil)
        if st != errSecSuccess { log("couldn't save the pairing key in the keychain (error \(st))") }
        var names = UserDefaults.standard.dictionary(forKey: namesKey) as? [String: String] ?? [:]
        names[hexString(clientID)] = name
        UserDefaults.standard.set(names, forKey: namesKey)
    }

    static func forget(_ clientID: [UInt8]) {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: hexString(clientID),
        ]
        SecItemDelete(q as CFDictionary)
        var names = UserDefaults.standard.dictionary(forKey: namesKey) as? [String: String] ?? [:]
        names[hexString(clientID)] = nil
        UserDefaults.standard.set(names, forKey: namesKey)
    }

    static func forgetAll() {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
        ]
        SecItemDelete(q as CFDictionary)
        UserDefaults.standard.removeObject(forKey: namesKey)
    }

    static var pairedNames: [String] {
        ((UserDefaults.standard.dictionary(forKey: namesKey) as? [String: String]) ?? [:]).values.sorted()
    }
}

extension Host {
    /// Reads HELLO (v1 or v2).
    func readHello(_ sock: Int32) -> Hello? {
        guard let msg = readMessage(sock, timeoutMs: 10_000, maxLen: 1024) else { return nil }
        let (type, p) = msg
        guard type == Int(G4_MSG_HELLO), p.count >= 12,
              p[0] == 0x47, p[1] == 0x34, p[2] == 0x44, p[3] == 0x50 else { return nil }
        var h = Hello()
        h.version = be16(p, 4)
        h.width = be16(p, 6); h.height = be16(p, 8); h.depth = Int(p[10]); h.flags = Int(p[11])
        h.slots = p.count >= 14 ? be16(p, 12) : 0
        h.widthMM = p.count >= 16 ? be16(p, 14) : 0          // 0 = unknown (older clients)
        if p.count >= Int(G4_HELLO_V2_LEN) {
            h.clientID = Array(p[16..<32])
            h.nonce = Array(p[32..<48])
            h.publicKey = Array(p[48..<80])
        }
        guard h.width > 0, h.height > 0, h.depth == 16 || h.depth == 32 else { return nil }
        return h
    }

    /// Prove to the PowerPC Mac that we're paired with it, and check that it
    /// is the Mac we paired with, or pair now (asking the user, who compares a
    /// code shown on both Macs). Nothing is captured or sent before this
    /// succeeds. Returns false to drop the connection.
    func authenticate(_ sock: Int32, hello: Hello, name: String) -> Bool {
        let hostID = PairingStore.hostID
        let clientHex = hexString(hello.clientID)
        var nh = [UInt8](repeating: 0, count: Int(G4_NONCE_BYTES))
        guard g4_random(&nh, Int(G4_NONCE_BYTES)) == 0 else { log("no randomness"); return false }

        var auth = [UInt8](repeating: 0, count: Int(G4_AUTH_LEN))
        auth.replaceSubrange(4..<20, with: hostID)
        auth.replaceSubrange(20..<36, with: nh)

        if let key = PairingStore.key(for: hello.clientID) {
            // --- already paired: exchange proofs ---
            auth[0] = UInt8(G4_AUTH_KNOWN)
            var tag = [UInt8](repeating: 0, count: Int(G4_TAG_BYTES))
            g4_auth_tag(&tag, key, CChar(72) /* 'H' */, hello.clientID, hostID, hello.nonce, nh)
            auth.replaceSubrange(36..<68, with: tag)
            guard writeAll(sock, message(type: Int(G4_MSG_AUTH), payload: auth)),
                  let reply = readMessage(sock, timeoutMs: 10_000, maxLen: 64) else {
                log("\(name) didn't answer the pairing check")
                return false
            }
            let (type, p) = reply
            if type == Int(G4_MSG_AUTH_FAIL) {
                // it doesn't know us any more (its pairings were reset): pair again next time
                log("\(name) no longer has this Mac paired; will ask to pair again")
                PairingStore.forget(hello.clientID)
                return false
            }
            return verifyClientProof(type, p, key: key, hello: hello, hostID: hostID, nh: nh, name: name)
        }

        // --- not paired yet: ask ---
        if declinedPairing.contains(clientHex) {
            post(.cantReach("You chose not to pair with “\(name)”. Quit and reopen PowerPC Display to be asked again."))
            return false
        }
        var pub = [UInt8](repeating: 0, count: Int(G4_PUB_BYTES))
        var priv = [UInt8](repeating: 0, count: Int(G4_PUB_BYTES))
        var key = [UInt8](repeating: 0, count: Int(G4_KEY_BYTES))
        defer { for i in priv.indices { priv[i] = 0 } }
        guard hello.publicKey.count == Int(G4_PUB_BYTES), g4_dh_keypair(&pub, &priv) == 0,
              g4_pair_key(&key, priv, hello.publicKey, hello.publicKey, pub, hello.clientID, hostID) == 0 else {
            log("pairing with \(name) failed (bad key)")
            return false
        }
        auth[0] = UInt8(G4_AUTH_PAIR)
        auth.replaceSubrange(36..<68, with: pub)
        guard writeAll(sock, message(type: Int(G4_MSG_AUTH), payload: auth)) else { return false }

        let code = g4_pair_code(key)
        log("\(name) wants to pair (code \(String(format: "%06u", code)))")
        post(.pairing(name: name, code: code))
        guard askToPair(name: name, code: code) else {
            log("pairing with \(name) declined here")
            declinedPairing.insert(clientHex)
            post(.cantReach("Not paired with “\(name)”."))
            return false
        }
        post(.pairing(name: name, code: code, waitingForOtherMac: true))
        guard let reply = readMessage(sock, timeoutMs: 120_000, maxLen: 64) else {
            post(.cantReach("Pairing with “\(name)” timed out. Click Pair on the PowerPC Mac too."))
            return false
        }
        let (type, p) = reply
        if type == Int(G4_MSG_AUTH_FAIL) {
            post(.cantReach("Pairing was declined on “\(name)”."))
            return false
        }
        guard verifyClientProof(type, p, key: key, hello: hello, hostID: hostID, nh: nh, name: name) else { return false }
        PairingStore.save(key: key, for: hello.clientID, name: name)
        log("paired with \(name)")
        return true
    }

    private func verifyClientProof(_ type: Int, _ p: [UInt8], key: [UInt8], hello: Hello,
                                   hostID: [UInt8], nh: [UInt8], name: String) -> Bool {
        var expect = [UInt8](repeating: 0, count: Int(G4_TAG_BYTES))
        g4_auth_tag(&expect, key, CChar(67) /* 'C' */, hello.clientID, hostID, hello.nonce, nh)
        guard type == Int(G4_MSG_AUTH_OK), p.count == Int(G4_AUTH_OK_LEN),
              g4_equal(expect, p, expect.count) == 1 else {
            log("\(name) failed the pairing check: not the PowerPC Mac this Mac paired with")
            post(.failed("“\(name)” isn't the PowerPC Mac you paired with, so nothing was sent to it."))
            return false
        }
        return true
    }

    /// Asks on the main thread. true = pair.
    private func askToPair(name: String, code: UInt32) -> Bool {
        var ok = false
        DispatchQueue.main.sync {
            NSApp.activate(ignoringOtherApps: true)
            let a = NSAlert()
            a.messageText = "Pair with “\(name)”?"
            a.informativeText = "Pair only if the PowerPC Mac shows the same code:\n\n"
                + String(format: "%03u %03u", code / 1000, code % 1000)
                + "\n\nThen click Pair on the PowerPC Mac too. Only paired Macs can receive your screen."
            a.addButton(withTitle: "Pair")
            a.addButton(withTitle: "Don't Pair")
            ok = a.runModal() == .alertFirstButtonReturn
        }
        return ok
    }
}

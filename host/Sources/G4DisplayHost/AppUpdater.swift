import AppKit
import Foundation

/// Updates from the GitHub releases of the project (the repository has to
/// be public: there's no sign-in). The newest release's Mac app replaces this
/// one; its Tiger app is kept for PowerPC Macs (see TigerUpdate).
final class AppUpdater {
    static let repo = "cbalcarek/powerpc-display"
    private static let lastCheckKey = "lastUpdateCheck"
    private var busy = false

    enum UpdateError: Error { case notVisible, failed(String) }

    struct Release {
        let version: String, name: String, notes: String
        let macURL: URL?, tigerURL: URL?
    }

    var currentVersion: String { Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0" }

    /// Main thread. Automatic checks happen at most once a day and stay quiet
    /// unless there's something to install.
    func check(userInitiated: Bool) {
        if busy { return }
        if !userInitiated, let last = UserDefaults.standard.object(forKey: Self.lastCheckKey) as? Date,
           Date().timeIntervalSince(last) < 24 * 3600 { return }
        busy = true
        Task.detached { [self] in
            defer { DispatchQueue.main.async { self.busy = false } }
            do {
                let rel = try await self.fetchNewest()
                DispatchQueue.main.async { UserDefaults.standard.set(Date(), forKey: Self.lastCheckKey) }
                guard let rel else {
                    if userInitiated { await self.tell("No releases yet", "There are no PowerPC Display releases on GitHub yet.") }
                    return
                }
                // keep the newest PowerPC app here so PowerPC Macs can update from this Mac
                if let t = rel.tigerURL { await self.fetchTiger(t, version: rel.version) }
                if versionNewer(rel.version, than: self.currentVersion), let mac = rel.macURL {
                    if await self.ask(rel) { try await self.install(mac, version: rel.version) }
                } else if userInitiated {
                    await self.tell("PowerPC Display is up to date", "Version \(self.currentVersion) is the newest.")
                }
            } catch UpdateError.notVisible {
                if userInitiated {
                    await self.tell("Can't see the releases",
                                    "GitHub didn't show the releases of \(Self.repo). If the repository is private, make it public (Settings › General › Danger Zone) and try again.")
                }
            } catch UpdateError.failed(let why) {
                log("update check: \(why)")
                if userInitiated { await self.tell("Couldn't update", why) }
            } catch {
                log("update check: \(error.localizedDescription)")
                if userInitiated { await self.tell("Couldn't check for updates", error.localizedDescription) }
            }
        }
    }

    // MARK: - GitHub

    /// The newest release that isn't a draft (pre-releases count).
    private func fetchNewest() async throws -> Release? {
        var req = URLRequest(url: URL(string: "https://api.github.com/repos/\(Self.repo)/releases?per_page=30")!)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.setValue("PowerPCDisplay/\(currentVersion)", forHTTPHeaderField: "User-Agent")
        req.timeoutInterval = 20
        let (data, resp) = try await URLSession.shared.data(for: req)
        guard let http = resp as? HTTPURLResponse else { throw UpdateError.failed("no answer from GitHub") }
        if http.statusCode == 404 { throw UpdateError.notVisible }
        guard http.statusCode == 200,
              let list = try JSONSerialization.jsonObject(with: data) as? [[String: Any]] else {
            throw UpdateError.failed("GitHub answered \(http.statusCode)")
        }
        var best: Release?
        for r in list where (r["draft"] as? Bool) != true {
            guard let tag = r["tag_name"] as? String else { continue }
            let v = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
            var mac: URL?, tiger: URL?
            for a in (r["assets"] as? [[String: Any]]) ?? [] {
                guard let n = a["name"] as? String, let s = a["browser_download_url"] as? String,
                      let u = URL(string: s) else { continue }
                if n == "PowerPC-Display-Mac.zip" { mac = u }
                if n == "PowerPC-Display-Tiger.zip" { tiger = u }
            }
            let rel = Release(version: v, name: (r["name"] as? String) ?? tag, notes: (r["body"] as? String) ?? "",
                              macURL: mac, tigerURL: tiger)
            if best == nil || versionNewer(v, than: best!.version) { best = rel }
        }
        return best
    }

    /// Download the release's Tiger app if it's newer than the one kept here.
    private func fetchTiger(_ url: URL, version: String) async {
        let have = TigerUpdate.version(of: TigerUpdate.downloadedZip)
        if let have, !versionNewer(version, than: have) { return }
        do {
            let (tmp, _) = try await URLSession.shared.download(from: url)
            guard let v = TigerUpdate.version(of: tmp) else { log("update: the release's Tiger zip isn't PowerPC Display"); return }
            let dst = TigerUpdate.downloadedZip
            try? FileManager.default.removeItem(at: dst)
            try FileManager.default.moveItem(at: tmp, to: dst)
            log("update: kept PowerPC Display \(v) for PowerPC Macs")
        } catch {
            log("update: couldn't download the Tiger app: \(error.localizedDescription)")
        }
    }

    // MARK: - installing

    private func install(_ url: URL, version: String) async throws {
        let (zip, _) = try await URLSession.shared.download(from: url)
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ppcdisplay-update-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        guard run("/usr/bin/ditto", ["-x", "-k", zip.path, dir.path]).status == 0 else {
            throw UpdateError.failed("The download couldn't be unpacked.")
        }
        let newApp = dir.appendingPathComponent("PowerPC Display.app")
        guard let info = NSDictionary(contentsOf: newApp.appendingPathComponent("Contents/Info.plist")),
              info["CFBundleIdentifier"] as? String == Bundle.main.bundleIdentifier,
              let nv = info["CFBundleShortVersionString"] as? String, versionNewer(nv, than: currentVersion) else {
            throw UpdateError.failed("The release's Mac zip doesn't contain a newer PowerPC Display.")
        }
        guard run("/usr/bin/codesign", ["--verify", "--deep", newApp.path]).status == 0 else {
            throw UpdateError.failed("The new app's signature is broken; nothing was changed.")
        }
        // Same signer as this app (your own build certificate)? Otherwise it
        // may not be yours, and macOS would also forget its Screen Recording
        // permission.
        if designated(newApp) != designated(Bundle.main.bundleURL) {
            let go = await confirm("This update is signed differently",
                                   "It wasn't built with the same certificate as the app you have now (built on another Mac?). Install it only if you made that release. macOS will ask for Screen Recording permission again.",
                                   ok: "Install Anyway")
            if !go { return }
        }
        _ = run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", newApp.path])
        let cur = Bundle.main.bundleURL
        do {
            _ = try FileManager.default.replaceItemAt(cur, withItemAt: newApp)
        } catch {
            throw UpdateError.failed("Couldn't replace \(cur.path): \(error.localizedDescription)")
        }
        log("update: installed \(nv), restarting")
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = ["-c", "sleep 1; /usr/bin/open \"$0\"", cur.path]
        try? p.run()
        DispatchQueue.main.async { NSApp.terminate(nil) }
    }

    private func designated(_ app: URL) -> String {
        run("/usr/bin/codesign", ["-d", "-r-", app.path]).output
            .split(separator: "\n").first { $0.hasPrefix("designated") }.map(String.init) ?? ""
    }

    private func run(_ tool: String, _ args: [String]) -> (status: Int32, output: String) {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: tool)
        p.arguments = args
        let out = Pipe()
        p.standardOutput = out
        p.standardError = out
        guard (try? p.run()) != nil else { return (-1, "") }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, String(decoding: data, as: UTF8.self))
    }

    // MARK: - asking (main thread)

    private func ask(_ rel: Release) async -> Bool {
        var notes = rel.notes.replacingOccurrences(of: "\r", with: "")
        if notes.count > 700 { notes = String(notes.prefix(700)) + "…" }
        return await confirm("PowerPC Display \(rel.version) is available",
                             "You have \(currentVersion).\n\n\(notes)", ok: "Install and Restart")
    }

    private func confirm(_ title: String, _ text: String, ok: String) async -> Bool {
        await withCheckedContinuation { c in
            DispatchQueue.main.async {
                NSApp.activate(ignoringOtherApps: true)
                let a = NSAlert()
                a.messageText = title
                a.informativeText = text
                a.addButton(withTitle: ok)
                a.addButton(withTitle: "Later")
                c.resume(returning: a.runModal() == .alertFirstButtonReturn)
            }
        }
    }

    private func tell(_ title: String, _ text: String) async {
        await withCheckedContinuation { (c: CheckedContinuation<Void, Never>) in
            DispatchQueue.main.async {
                NSApp.activate(ignoringOtherApps: true)
                let a = NSAlert()
                a.messageText = title
                a.informativeText = text
                a.runModal()
                c.resume()
            }
        }
    }
}

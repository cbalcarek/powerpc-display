import SwiftUI

// The status window: a list of PowerPC Macs (like Bluetooth in System
// Settings), the selected Mac's details, then the settings in groups.

struct StatusView: View {
    @ObservedObject var model: AppModel
    let controller: AppController

    private var version: String {
        Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "development"
    }

    var body: some View {
        let active = model.peers.filter(\.isActive)
        let others = model.peers.filter { !$0.isActive }
        let selected = model.selected

        VStack(alignment: .leading, spacing: 18) {
            BoxSection("My PowerPC Macs") {
                if active.isEmpty {
                    HStack(spacing: 12) {
                        ProgressView().controlSize(.small).frame(width: 34)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(model.peers.isEmpty ? "Looking for PowerPC Macs…" : "None connected")
                            Text("Open PowerPC Display on the G3, G4 or G5. Both Macs need an Ethernet cable between them or the same network.")
                                .font(.caption).foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    .padding(.horizontal, 14).padding(.vertical, 10)
                }
                peerRows(active, selected: selected)
            }

            if !others.isEmpty {
                BoxSection("Other PowerPC Macs") { peerRows(others, selected: selected) }
            }

            if let selected {
                PeerDetail(peer: selected, controller: controller)
                    .id(selected.id)
            }

            BoxSection("Video") {
                HStack {
                    Text("Frame rate")
                    Spacer()
                    Picker("Frame rate", selection: Binding(get: { model.fps }, set: { controller.setFPS($0) })) {
                        ForEach(AppController.fpsChoices, id: \.self) { Text("\($0) fps").tag($0) }
                    }
                    .labelsHidden()
                    .fixedSize()
                }
                .padding(.horizontal, 14).padding(.vertical, 8)
                BoxDivider()
                SwitchRow("Smoother video",
                          caption: "Slightly softer colour in videos; text stays sharp",
                          isOn: Binding(get: { model.smoothVideo }, set: { controller.setSmoothVideo($0) }))
                    .help("Areas playing video are sent with half the data. Text and still areas stay exact.")
                BoxDivider()
                SwitchRow("Match video quality to the network",
                          caption: "As sharp as the connection allows",
                          isOn: Binding(get: { model.fastVideo }, set: { controller.setFastVideo($0) }))
                    .disabled(!model.smoothVideo)
                    .help("Video is sent as sharp as the connection allows: lossless when there's room, softer colour, or half resolution when the network or the PowerPC Mac can't keep up. Off: always softer colour, full resolution.")
            }

            BoxSection("General") {
                SwitchRow("Match this Mac's brightness",
                          isOn: Binding(get: { model.matchBrightness }, set: { controller.setMatchBrightness($0) }))
                    .help("PowerPC Macs with a built-in screen (iMac, iBook, PowerBook) follow this Mac's brightness while they show it, and go back to their own afterwards.")
                if controller.isBundled {
                    BoxDivider()
                    SwitchRow("Launch at login",
                              isOn: Binding(get: { model.launchAtLogin }, set: { controller.setLaunchAtLogin($0) }))
                }
            }

            HStack {
                Text("Version \(version)")
                Spacer()
                Button("Check for Updates…") { controller.checkForUpdates() }
                    .buttonStyle(.link)
            }
            .font(.caption)
            .foregroundStyle(.secondary)
            .padding(.horizontal, 4)
        }
        .padding(20)
        .frame(width: 520)
    }

    @ViewBuilder
    private func peerRows(_ peers: [PeerModel], selected: PeerModel?) -> some View {
        ForEach(Array(peers.enumerated()), id: \.element.id) { i, peer in
            if i > 0 { BoxDivider() }
            PeerListRow(peer: peer, isSelected: peer === selected, controller: controller) {
                model.selectedID = peer.id
            }
        }
    }
}

/// One PowerPC Mac in the list. Click to show its details; right-click for
/// the same actions.
struct PeerListRow: View {
    @ObservedObject var peer: PeerModel
    let isSelected: Bool
    let controller: AppController
    let select: () -> Void

    var body: some View {
        Button(action: select) {
            HStack(spacing: 12) {
                Image(systemName: peer.deviceSymbol)
                    .font(.system(size: 22, weight: .light))
                    .foregroundStyle(peer.isActive ? Color.primary : Color.secondary)
                    .frame(width: 34)
                VStack(alignment: .leading, spacing: 2) {
                    Text(peer.displayName)
                        .foregroundStyle(peer.isActive ? Color.primary : Color.secondary)
                    Text(peer.subtitle).font(.caption).foregroundStyle(.secondary)
                }
                Spacer(minLength: 8)
                HStack(spacing: 6) {
                    StatusDot(peer: peer)
                    Text(peer.shortStatus).font(.caption).foregroundStyle(.secondary)
                }
                Image(systemName: "info.circle").foregroundStyle(.secondary)
            }
            .padding(.horizontal, 14).padding(.vertical, 9)
            .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
            .contentShape(Rectangle())
            .background(isSelected ? Color.accentColor.opacity(0.18) : .clear)
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(peer.displayName), \(peer.shortStatus)")
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        .contextMenu {
            if peer.isConnected {
                Button("Disconnect") { controller.setConnected(false, peer) }
            } else if peer.isPaused {
                Button("Connect") { controller.setConnected(true, peer) }
            }
            if !peer.isActive, peer.host.canWake { Button("Wake") { controller.wake(peer) } }
            if !peer.isActive, peer.name != nil {
                Divider()
                Button("Remove from List…") { controller.forget(peer) }
            }
        }
    }
}

struct StatusDot: View {
    @ObservedObject var peer: PeerModel
    var body: some View {
        if peer.isConnected {
            Circle().fill(Color.green).frame(width: 8, height: 8)
        } else if peer.isPairing {
            Circle().fill(Color.orange).frame(width: 8, height: 8)
        } else {
            Circle().strokeBorder(Color.secondary, lineWidth: 1.5).frame(width: 8, height: 8)
        }
    }
}

/// The selected PowerPC Mac: live numbers, Extend/Mirror and what you can do.
struct PeerDetail: View {
    @ObservedObject var peer: PeerModel
    let controller: AppController

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(peer.displayName).font(.headline)
                Spacer()
                if case .connected(_, let w, let h, let d) = peer.status {
                    Text("\(w) × \(h) · \(d)-bit colour").font(.caption).foregroundStyle(.secondary)
                }
            }

            if peer.isConnected {
                TimelineView(.periodic(from: .now, by: 1)) { _ in
                    if let s = peer.liveStats(now: nowMs()) {
                        HStack(spacing: 8) {
                            tile(String(format: "%.0f", s.fps), "fps")
                            tile(String(format: "%.0f", s.kbps), "KB/s")
                            tile(String(format: "%.0f", s.latencyMs), "ms to screen")
                            tile(s.videoQuality ?? "—", "video")
                        }
                    } else {
                        Text("Idle — nothing on its screen is changing")
                            .font(.callout).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, minHeight: 52, alignment: .leading)
                    }
                }
            } else {
                Text(peer.detail)
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            if !peer.isPairing {
                HStack {
                    Text("Show")
                    Spacer()
                    Picker("Show", selection: Binding(get: { peer.mirror }, set: { controller.setMirror($0, peer) })) {
                        Text("Extend").tag(false)
                        Text("Mirror").tag(true)
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    .fixedSize()
                }
            }

            HStack(spacing: 8) {
                if case .needsScreenRecording = peer.status {
                    Button("Open Screen Recording Settings…") { controller.openPrivacySettings() }
                }
                Spacer()
                if peer.isConnected {
                    Button("Arrange Displays…") { controller.openDisplaySettings() }
                        .disabled(peer.mirror)
                        .help("Place it next to your other displays (not needed while mirroring)")
                    Button("Disconnect") { controller.setConnected(false, peer) }
                } else if peer.isPaused {
                    Button("Connect") { controller.setConnected(true, peer) }
                        .keyboardShortcut(.defaultAction)
                }
                if !peer.isActive, peer.host.canWake, !peer.isPaused {
                    Button("Wake") { controller.wake(peer) }
                        .help("Wake it over the network (Wake-on-LAN)")
                }
                if !peer.isActive, peer.name != nil {
                    Button("Remove from List…") { controller.forget(peer) }
                }
            }
        }
        .padding(16)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.04)))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color.primary.opacity(0.1)))
    }

    private func tile(_ value: String, _ label: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(value).font(.system(size: 20, weight: .semibold, design: .rounded)).monospacedDigit()
                .lineLimit(1).minimumScaleFactor(0.7)
            Text(label).font(.caption).foregroundStyle(.secondary).lineLimit(1)
        }
        .padding(.horizontal, 12).padding(.vertical, 9)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.primary.opacity(0.05)))
    }
}

// MARK: - building blocks (grouped boxes like System Settings)

struct BoxSection<Content: View>: View {
    let title: String
    let content: Content

    init(_ title: String, @ViewBuilder content: () -> Content) {
        self.title = title
        self.content = content()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.subheadline.weight(.semibold))
                .foregroundStyle(.secondary)
                .padding(.leading, 4)
                .accessibilityAddTraits(.isHeader)
            VStack(alignment: .leading, spacing: 0) { content }
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.05)))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color.primary.opacity(0.08)))
                .clipShape(RoundedRectangle(cornerRadius: 10))
        }
    }
}

struct BoxDivider: View {
    var body: some View { Divider().padding(.leading, 14) }
}

/// A setting with an optional explanation underneath and a switch on the right.
struct SwitchRow: View {
    let title: String
    let caption: String?
    @Binding var isOn: Bool

    init(_ title: String, caption: String? = nil, isOn: Binding<Bool>) {
        self.title = title
        self.caption = caption
        _isOn = isOn
    }

    var body: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                if let caption { Text(caption).font(.caption).foregroundStyle(.secondary) }
            }
            Spacer()
            Toggle(title, isOn: $isOn)
                .labelsHidden()
                .toggleStyle(.switch)
        }
        .padding(.horizontal, 14).padding(.vertical, 8)
    }
}

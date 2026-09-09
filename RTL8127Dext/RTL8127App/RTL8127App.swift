// SPDX-License-Identifier: GPL-2.0-only
//
// RTL8127App.swift -- host app for the RTL8127 DriverKit extension.
//
// DriverKit extensions can only be activated from an app that embeds them
// (Contents/Library/SystemExtensions). This app does the whole job for the
// user: it activates the extension on launch, opens the right System
// Settings pane when macOS asks for approval, and then shows whether a
// Realtek card is present, whether the driver is attached to it, and the
// state of the network link.

import SwiftUI
import SystemExtensions
import IOKit
import AppKit

// MARK: - Extension state

enum DriverState: Equatable {
    case checking
    case installing
    case waitingForApproval
    case enabled
    case rebootRequired
    case failed(String)
}

// MARK: - Hardware state (from the IORegistry)

struct CardInfo: Equatable {
    var model: String
    var deviceID: UInt16
    var location: String      // "Thunderbolt" or "PCIe"
    var driver: String?       // kernel-side driver class attached to the device
    var interface: String?    // en10
    var linkUp: Bool?
    var speedMbps: Int?
    var ipv4: String?
}

enum Hardware {
    static let models: [UInt16: String] = [
        0x8127: "RTL8127 (10GBASE-T)",
        0x0e10: "RTL8127ATF (SFP+)",
        0x8126: "RTL8126 (5GbE)",
        0x5000: "RTL8126 (5GbE)",
        0x8125: "RTL8125 (2.5GbE)",
        0x3000: "RTL8125 (2.5GbE)",
    ]

    static func property(_ entry: io_registry_entry_t, _ key: String) -> Any? {
        IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue()
    }

    static func u16(_ entry: io_registry_entry_t, _ key: String) -> UInt16? {
        guard let d = property(entry, key) as? Data, d.count >= 2 else { return nil }
        return UInt16(d[d.startIndex]) | UInt16(d[d.startIndex + 1]) << 8
    }

    static func scan() -> [CardInfo] {
        var iterator: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault,
                                           IOServiceMatching("IOPCIDevice"),
                                           &iterator) == KERN_SUCCESS else { return [] }
        defer { IOObjectRelease(iterator) }

        var cards: [CardInfo] = []
        while true {
            let dev = IOIteratorNext(iterator)
            if dev == 0 { break }
            defer { IOObjectRelease(dev) }
            guard u16(dev, "vendor-id") == 0x10ec,
                  let devID = u16(dev, "device-id"),
                  let model = models[devID] else { continue }

            var info = CardInfo(model: model, deviceID: devID, location: "PCIe")
            if property(dev, "IOPCITunnelled") as? Bool == true { info.location = "Thunderbolt" }
            walk(dev, into: &info)
            if let ifname = info.interface { info.ipv4 = ipv4Address(of: ifname) }
            cards.append(info)
        }
        return cards
    }

    /// Walk the device's children looking for the driver, the network
    /// interface and the link state.
    private static func walk(_ entry: io_registry_entry_t, into info: inout CardInfo) {
        var children: io_iterator_t = 0
        guard IORegistryEntryGetChildIterator(entry, kIOServicePlane, &children) == KERN_SUCCESS
        else { return }
        defer { IOObjectRelease(children) }

        while true {
            let child = IOIteratorNext(children)
            if child == 0 { break }
            defer { IOObjectRelease(child) }

            if info.driver == nil {
                if let userClass = property(child, "IOUserClass") as? String {
                    info.driver = userClass
                } else if IOObjectConformsTo(child, "IONetworkController") != 0 {
                    var name = [CChar](repeating: 0, count: 128)
                    IOObjectGetClass(child, &name)
                    info.driver = String(cString: name)
                }
            }
            if let status = property(child, "IOLinkStatus") as? Int {
                info.linkUp = (status & 0x2) != 0           // kIONetworkLinkActive
                if let bps = property(child, "IOLinkSpeed") as? Int, bps > 0 {
                    info.speedMbps = bps / 1_000_000
                }
            }
            if let bsd = property(child, "BSD Name") as? String { info.interface = bsd }
            walk(child, into: &info)
        }
    }

    private static func ipv4Address(of ifname: String) -> String? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        var p: UnsafeMutablePointer<ifaddrs>? = first
        while let cur = p {
            defer { p = cur.pointee.ifa_next }
            guard String(cString: cur.pointee.ifa_name) == ifname,
                  let addr = cur.pointee.ifa_addr,
                  addr.pointee.sa_family == UInt8(AF_INET) else { continue }
            var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
            if getnameinfo(addr, socklen_t(addr.pointee.sa_len), &host, socklen_t(host.count),
                           nil, 0, NI_NUMERICHOST) == 0 {
                return String(cString: host)
            }
        }
        return nil
    }
}

// MARK: - Manager

final class DriverManager: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let dextIdentifier = "net.wizzz.RTL8127Dext"
    static let settingsURL = URL(string:
        "x-apple.systempreferences:com.apple.LoginItems-Settings.extension"
        + "?extensionPointIdentifier=com.apple.system_extension.driver_extension")!

    @Published var state: DriverState = .checking
    @Published var cards: [CardInfo] = []

    private var timer: Timer?
    private var openedSettings = false

    func start() {
        refreshHardware()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
            self?.refreshHardware()
        }
        // Find out whether the extension is already installed; activate if not.
        let request = OSSystemExtensionRequest.propertiesRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func refreshHardware() {
        let found = Hardware.scan()
        if found != cards { cards = found }
    }

    func activate() {
        state = .installing
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        state = .installing
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func openSettings() {
        NSWorkspace.shared.open(Self.settingsURL)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 foundProperties properties: [OSSystemExtensionProperties]) {
        if properties.contains(where: { $0.isEnabled && !$0.isUninstalling }) {
            state = .enabled
        } else if properties.contains(where: { $0.isAwaitingUserApproval }) {
            state = .waitingForApproval
            if !openedSettings { openedSettings = true; openSettings() }
        } else {
            activate()
        }
    }

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        state = .waitingForApproval
        if !openedSettings { openedSettings = true; openSettings() }
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            state = .enabled
        case .willCompleteAfterReboot:
            state = .rebootRequired
        @unknown default:
            state = .enabled
        }
        refreshHardware()
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        let ns = error as NSError
        if ns.domain == OSSystemExtensionErrorDomain,
           ns.code == OSSystemExtensionError.requestCanceled.rawValue {
            // Typical for a deactivation request the user canceled.
            state = .enabled
            return
        }
        state = .failed(error.localizedDescription)
    }
}

// MARK: - UI

struct StatusRow<Detail: View>: View {
    let icon: String
    let color: Color
    let title: LocalizedStringKey
    let detail: Detail

    init(icon: String, color: Color, title: LocalizedStringKey, @ViewBuilder detail: () -> Detail) {
        self.icon = icon; self.color = color; self.title = title; self.detail = detail()
    }

    var body: some View {
        HStack(alignment: .top, spacing: 14) {
            Image(systemName: icon)
                .font(.title2)
                .foregroundStyle(color)
                .frame(width: 28)
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.headline)
                detail
            }
            Spacer(minLength: 0)
        }
    }
}

struct ContentView: View {
    @ObservedObject var manager: DriverManager

    var body: some View {
        VStack(alignment: .leading, spacing: 20) {
            VStack(alignment: .leading, spacing: 4) {
                Text("RTL8127 10GbE Driver").font(.title).bold()
                Text("Realtek RTL8127 / RTL8127ATF (SFP+), RTL8126 and RTL8125 network cards.")
                    .foregroundStyle(.secondary)
            }

            driverRow
            Divider()
            cardRow
            Divider()
            linkRow

            HStack {
                Link("Help and reporting", destination: URL(string: "https://github.com/stefb69/RTL812xLucy")!)
                    .font(.footnote)
                Spacer()
                if manager.state == .enabled {
                    Button("Remove driver…") { manager.deactivate() }
                        .font(.footnote)
                }
            }
            .padding(.top, 4)
        }
        .padding(24)
        .frame(width: 520)
        .onAppear { manager.start() }
    }

    // Step 1: the extension itself.
    @ViewBuilder private var driverRow: some View {
        switch manager.state {
        case .checking:
            StatusRow(icon: "circle.dotted", color: .secondary, title: "Driver") {
                Text("Checking…").foregroundStyle(.secondary)
            }
        case .installing:
            StatusRow(icon: "arrow.down.circle", color: .blue, title: "Driver") {
                HStack(spacing: 8) { ProgressView().controlSize(.small); Text("Installing…") }
                    .foregroundStyle(.secondary)
            }
        case .waitingForApproval:
            StatusRow(icon: "exclamationmark.circle.fill", color: .orange, title: "Driver needs your approval") {
                Text("macOS asks you to allow it once. In System Settings, turn on the switch next to **RTL8127App**, then come back here.")
                Button("Open System Settings") { manager.openSettings() }
                    .buttonStyle(.borderedProminent)
                    .padding(.top, 4)
            }
        case .enabled:
            StatusRow(icon: "checkmark.circle.fill", color: .green, title: "Driver installed") {
                Text("Loads automatically whenever a supported card is connected.")
                    .foregroundStyle(.secondary)
            }
        case .rebootRequired:
            StatusRow(icon: "arrow.clockwise.circle.fill", color: .orange, title: "Restart required") {
                Text("The driver will be active after you restart your Mac.")
            }
        case .failed(let message):
            StatusRow(icon: "xmark.circle.fill", color: .red, title: "Installation failed") {
                Text(message).font(.footnote.monospaced())
                Button("Try again") { manager.activate() }.padding(.top, 4)
            }
        }
    }

    // Step 2: is a card present, and is the driver attached to it.
    @ViewBuilder private var cardRow: some View {
        if let card = manager.cards.first {
            let attached = card.driver != nil
            StatusRow(icon: attached ? "checkmark.circle.fill" : "circle.dotted",
                      color: attached ? .green : .secondary,
                      title: "\(card.model) detected") {
                if attached {
                    Text("Connected via \(card.location), driver attached.")
                        .foregroundStyle(.secondary)
                } else if manager.state == .enabled {
                    Text("Connected via \(card.location). Driver not attached yet — this can take a few seconds. If it stays like this, unplug and replug the card.")
                        .foregroundStyle(.secondary)
                } else {
                    Text("Connected via \(card.location). Finish the driver installation above.")
                        .foregroundStyle(.secondary)
                }
            }
        } else {
            StatusRow(icon: "circle.dotted", color: .secondary, title: "No card detected") {
                Text("Plug in the card (or the Thunderbolt enclosure holding it). It will show up here automatically.")
                    .foregroundStyle(.secondary)
            }
        }
    }

    // Step 3: the network link.
    @ViewBuilder private var linkRow: some View {
        if let card = manager.cards.first, card.driver != nil {
            let up = card.linkUp ?? false
            StatusRow(icon: up ? "network" : "network.slash",
                      color: up ? .green : .orange,
                      title: up ? "Link up" : "No link") {
                if up {
                    HStack(spacing: 6) {
                        if let mbps = card.speedMbps { Text(speedText(mbps)).bold() }
                        if let name = card.interface { Text("· \(name)") }
                        if let ip = card.ipv4 { Text("· \(ip)") }
                    }
                    .foregroundStyle(.secondary)
                    Text("Configure it like any Ethernet port in System Settings › Network.")
                        .font(.footnote).foregroundStyle(.secondary)
                } else {
                    Text("Check the cable, the SFP+ module or DAC, and the switch port.")
                        .foregroundStyle(.secondary)
                }
            }
        } else {
            StatusRow(icon: "network", color: .secondary, title: "Network") {
                Text("Link status appears once the driver is attached to a card.")
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func speedText(_ mbps: Int) -> LocalizedStringKey {
        mbps >= 1000 ? "\(mbps / 1000) Gbit/s" : "\(mbps) Mbit/s"
    }
}

@main
struct RTL8127App: App {
    @StateObject private var manager = DriverManager()

    var body: some Scene {
        WindowGroup {
            ContentView(manager: manager)
        }
        .windowResizability(.contentSize)
    }
}

// SPDX-License-Identifier: GPL-2.0-only
//
// RTL8127App.swift -- minimal host app for the RTL8127 DriverKit extension.
//
// DriverKit extensions can only be activated from an app that embeds them
// (Contents/Library/SystemExtensions). This app submits the
// OSSystemExtensionRequest and shows the activation state.

import SwiftUI
import SystemExtensions

final class ExtensionManager: NSObject, ObservableObject, OSSystemExtensionRequestDelegate {
    static let dextIdentifier = "net.wizzz.RTL8127Dext"

    @Published var status = "Inactif"

    func activate() {
        status = "Activation en cours…"
        let request = OSSystemExtensionRequest.activationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    func deactivate() {
        status = "Désactivation en cours…"
        let request = OSSystemExtensionRequest.deactivationRequest(
            forExtensionWithIdentifier: Self.dextIdentifier, queue: .main)
        request.delegate = self
        OSSystemExtensionManager.shared.submitRequest(request)
    }

    // MARK: OSSystemExtensionRequestDelegate

    func request(_ request: OSSystemExtensionRequest,
                 actionForReplacingExtension existing: OSSystemExtensionProperties,
                 withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
        status = "Remplacement \(existing.bundleShortVersion) -> \(ext.bundleShortVersion)"
        return .replace
    }

    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        status = "Approbation requise : Réglages Système > Confidentialité et sécurité"
    }

    func request(_ request: OSSystemExtensionRequest,
                 didFinishWithResult result: OSSystemExtensionRequest.Result) {
        switch result {
        case .completed:
            status = "Extension activée"
        case .willCompleteAfterReboot:
            status = "Activée après redémarrage"
        @unknown default:
            status = "Résultat inconnu (\(result.rawValue))"
        }
    }

    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        status = "Échec : \(error.localizedDescription)"
    }
}

struct ContentView: View {
    @ObservedObject var manager: ExtensionManager

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("RTL8127 10GbE DriverKit")
                .font(.title2).bold()
            Text("Pilote pour les contrôleurs Realtek RTL8127, dont la variante fibre RTL8127ATF (SFP+).")
                .font(.callout)
                .foregroundStyle(.secondary)

            HStack(spacing: 12) {
                Button("Activer le pilote") { manager.activate() }
                    .buttonStyle(.borderedProminent)
                Button("Désactiver") { manager.deactivate() }
            }

            Divider()
            Text(manager.status)
                .font(.footnote.monospaced())
        }
        .padding(24)
        .frame(minWidth: 460)
    }
}

@main
struct RTL8127App: App {
    @StateObject private var manager = ExtensionManager()

    var body: some Scene {
        WindowGroup {
            ContentView(manager: manager)
        }
        .windowResizability(.contentSize)
    }
}

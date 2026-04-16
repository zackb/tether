//
//  BonjourDiscovery.swift
//  Tether
//
//  NWBrowser-based mDNS/Bonjour discovery for `_tether._tcp` services.
//

import Foundation
import Network

/// A single discovered `tetherd` instance on the local network.
struct DiscoveredHost: Identifiable, Sendable {
    let id = UUID()
    let name: String
    let endpoint: NWEndpoint
    let fingerprint: String
}

/// Scans the local network for `tetherd` daemons advertising `_tether._tcp`
/// via Bonjour/mDNS and exposes the results as an observable list.
@Observable
final class BonjourDiscovery {
    private static let serviceType = "_tether._tcp"

    /// Our own fingerprint, to filter ourselves out of the results.
    var localFingerprint: String?

    /// Our own device name, as a fallback filter if the TXT record hasn't arrived.
    var localDeviceName: String?

    /// Currently discovered hosts (updated live as services appear/disappear).
    private(set) var hosts: [DiscoveredHost] = []

    /// Whether the browser is actively scanning.
    private(set) var isScanning = false

    private var browser: NWBrowser?
    var onHostsChanged: (([DiscoveredHost]) -> Void)?

    // MARK: - Public

    /// Start scanning for `_tether._tcp` services.
    func startScanning(forceRestart: Bool = false) {
        if forceRestart {
            stopScanning(clearHosts: true)
        }

        guard !isScanning else { return }

        let params = NWParameters()
        params.includePeerToPeer = true

        let browser = NWBrowser(
            for: .bonjour(type: Self.serviceType, domain: nil),
            using: params
        )

        browser.stateUpdateHandler = { [weak self] state in
            switch state {
            case .ready:
                self?.isScanning = true
            case .failed(let error):
                print("BonjourDiscovery: Browser failed: \(error)")
                self?.isScanning = false
                self?.browser = nil
            case .cancelled:
                self?.isScanning = false
                self?.browser = nil
            default:
                break
            }
        }

        browser.browseResultsChangedHandler = { [weak self] results, _ in
            self?.handleResults(results)
        }

        browser.start(queue: .main)
        self.browser = browser
    }

    /// Stop scanning.
    func stopScanning(clearHosts: Bool = false) {
        browser?.cancel()
        browser = nil
        isScanning = false
        if clearHosts {
            hosts = []
            onHostsChanged?(hosts)
        }
    }

    // MARK: - Private

    private func handleResults(_ results: Set<NWBrowser.Result>) {
        hosts = results.compactMap { result -> DiscoveredHost? in
            guard case .service(let name, _, _, _) = result.endpoint else {
                return nil
            }

            let fingerprint = extractFingerprint(from: result.metadata)
            
            if let local = self.localFingerprint, !local.isEmpty, fingerprint == local {
                return nil
            }
            
            if let localName = self.localDeviceName, !localName.isEmpty, name == localName {
                return nil
            }
            
            return DiscoveredHost(
                name: name,
                endpoint: result.endpoint,
                fingerprint: fingerprint
            )
        }
        onHostsChanged?(hosts)
    }

    private func extractFingerprint(from metadata: NWBrowser.Result.Metadata) -> String {
        guard case .bonjour(let txtRecord) = metadata else {
            return ""
        }

        if let fpEntry = txtRecord.getEntry(for: "fp") {
            switch fpEntry {
            case .string(let value):
                return value
            case .data(let data):
                return String(data: data, encoding: .utf8) ?? ""
            @unknown default:
                return ""
            }
        }
        return ""
    }
}

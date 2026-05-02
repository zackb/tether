//
//  BonjourDiscovery.swift
//  TetherFramework
//
//  NetServiceBrowser-based mDNS/Bonjour discovery for `_tether._tcp` services.
//  Using NetService to correctly parse TXT records which NWBrowser ignores.
//

import Foundation
import Network

// A single discovered `tetherd` instance on the local network.
public struct DiscoveredHost: Identifiable, Sendable {
    public let id = UUID()
    public let name: String
    public let endpoint: NWEndpoint
    public let fingerprint: String

    public init(name: String, endpoint: NWEndpoint, fingerprint: String) {
        self.name = name
        self.endpoint = endpoint
        self.fingerprint = fingerprint
    }
}

// Scans the local network for `tetherd` daemons advertising `_tether._tcp`
// via Bonjour/mDNS and exposes the results as an observable list.
@Observable
public final class BonjourDiscovery: NSObject, NetServiceBrowserDelegate, NetServiceDelegate {
    private static let serviceType = "_tether._tcp"
    private static let serviceDomain = "local."

    // Our own fingerprint, to filter ourselves out of the results.
    public var localFingerprint: String?

    // Our own device name, as a fallback filter if the TXT record hasn't arrived.
    public var localDeviceName: String?

    // Currently discovered hosts (updated live as services appear/disappear).
    public private(set) var hosts: [DiscoveredHost] = []

    // Whether the browser is actively scanning.
    public private(set) var isScanning = false

    private var browser: NetServiceBrowser?
    private var activeServices: Set<NetService> = []

    @ObservationIgnored public var onHostsChanged: (([DiscoveredHost]) -> Void)?

    public override init() {
        super.init()
    }

    // MARK: - Public

    // Start scanning for `_tether._tcp` services.
    public func startScanning(forceRestart: Bool = false) {
        if forceRestart {
            stopScanning(clearHosts: true)
        }

        guard !isScanning else { return }

        isScanning = true
        let newBrowser = NetServiceBrowser()
        newBrowser.delegate = self
        newBrowser.includesPeerToPeer = true
        newBrowser.searchForServices(ofType: Self.serviceType, inDomain: Self.serviceDomain)
        browser = newBrowser
    }

    // Stop scanning.
    public func stopScanning(clearHosts: Bool = false) {
        browser?.stop()
        browser = nil
        isScanning = false
        activeServices.forEach { $0.stop() }
        activeServices.removeAll()

        if clearHosts {
            hosts = []
            onHostsChanged?(hosts)
        }
    }

    // MARK: - NetServiceBrowserDelegate

    public func netServiceBrowser(_ browser: NetServiceBrowser, didFind service: NetService, moreComing: Bool) {
        activeServices.insert(service)
        service.delegate = self
        service.resolve(withTimeout: 5.0)
    }

    public func netServiceBrowser(_ browser: NetServiceBrowser, didRemove service: NetService, moreComing: Bool) {
        activeServices.remove(service)

        hosts.removeAll { $0.name == service.name }
        onHostsChanged?(hosts)
    }

    public func netServiceBrowser(_ browser: NetServiceBrowser, didNotSearch errorDict: [String: NSNumber]) {
        print("BonjourDiscovery: Browser failed to search: \(errorDict)")
        isScanning = false
        self.browser = nil
    }

    public func netServiceBrowserDidStopSearch(_ browser: NetServiceBrowser) {
        isScanning = false
    }

    // MARK: - NetServiceDelegate

    public func netServiceDidResolveAddress(_ sender: NetService) {
        let name = sender.name
        let endpoint = NWEndpoint.service(name: name, type: Self.serviceType, domain: Self.serviceDomain, interface: nil)
        var fingerprint = ""

        if let txtData = sender.txtRecordData() {
            let dict = NetService.dictionary(fromTXTRecord: txtData)
            if let fpData = dict["fp"], let fpString = String(data: fpData, encoding: .utf8) {
                fingerprint = fpString
            }
        }

        print("BonjourDiscovery: Discovered host: \(name), fingerprint: '\(fingerprint)'")

        if let local = self.localFingerprint, !local.isEmpty, fingerprint == local {
            return
        }

        if let localName = self.localDeviceName, !localName.isEmpty, name == localName {
            return
        }

        if let index = hosts.firstIndex(where: { $0.name == name }) {
            hosts[index] = DiscoveredHost(name: name, endpoint: endpoint, fingerprint: fingerprint)
        } else {
            hosts.append(DiscoveredHost(name: name, endpoint: endpoint, fingerprint: fingerprint))
        }

        onHostsChanged?(hosts)
    }

    public func netService(_ sender: NetService, didUpdateTXTRecord data: Data) {
        netServiceDidResolveAddress(sender)
    }
}

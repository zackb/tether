//
//  TetherServer.swift
//  TetherFramework
//
//  NWListener-based TCP + TLS server to accept incoming connections
//  from `tetherd` instances (making iOS a symmetrical peer).
//

import Foundation
import Network
import Security

@Observable
public final class TetherServer: NSObject, NetServiceDelegate {
    private var listener: NWListener?
    private var netService: NetService?

    public var isListening = false

    // Callback when a verified inbound connection is received
    public var onNewConnection: ((NWConnection, String) -> Void)?

    public override init() {
        super.init()
    }

    public func start(identity: SecIdentity, localDeviceName: String, fingerprint: String) {
        stop()

        let tlsOptions = NWProtocolTLS.Options()
        let secOptions = tlsOptions.securityProtocolOptions

        let secIdentity = sec_identity_create(identity)!
        sec_protocol_options_set_local_identity(secOptions, secIdentity)

        sec_protocol_options_set_min_tls_protocol_version(secOptions, .TLSv12)
        sec_protocol_options_set_max_tls_protocol_version(secOptions, .TLSv12)

        // As a server, require the client to present its certificate
        sec_protocol_options_set_peer_authentication_required(secOptions, true)

        // Temporarily store the fingerprint. In a high-concurrency server we'd use a robust map,
        // but for a single peer-to-peer mobile app, a simple property is more than sufficient.
        var temporaryHandshakeFingerprint = ""

        // Accept the certificate and capture the fingerprint
        sec_protocol_options_set_verify_block(secOptions, { metadata, trust, completion in
            let secTrust = sec_trust_copy_ref(trust).takeRetainedValue()
            if let certChain = SecTrustCopyCertificateChain(secTrust) as? [SecCertificate],
               let clientCert = certChain.first {
                temporaryHandshakeFingerprint = CertificateManager.fingerprint(ofCertificate: clientCert)
            }
            completion(true)
        }, .main)

        let params = NWParameters(tls: tlsOptions, tcp: .init())
        params.includePeerToPeer = true

        do {
            let nwListener = try NWListener(using: params)

            nwListener.newConnectionHandler = { [weak self] connection in
                self?.handleNewConnection(connection, capturedFingerprint: temporaryHandshakeFingerprint)
            }

            nwListener.stateUpdateHandler = { [weak self] state in
                guard let self = self else { return }
                switch state {
                case .ready:
                    self.isListening = true
                    if let port = nwListener.port?.rawValue {
                        print("TetherServer: Listening on port \(port)")
                        self.publishBonjour(port: port, deviceName: localDeviceName, fingerprint: fingerprint)
                    }
                case .failed(let error):
                    print("TetherServer: Listener failed with error: \(error)")
                    self.isListening = false
                    self.stop()
                case .cancelled:
                    self.isListening = false
                default:
                    break
                }
            }

            nwListener.start(queue: .main)
            self.listener = nwListener

        } catch {
            print("TetherServer: Failed to create listener: \(error)")
        }
    }

    public func stop() {
        listener?.cancel()
        listener = nil

        netService?.stop()
        netService = nil

        isListening = false
    }

    private func publishBonjour(port: UInt16, deviceName: String, fingerprint: String) {
        netService = NetService(domain: "local.", type: "_tether._tcp", name: deviceName, port: Int32(port))

        if let fpData = fingerprint.data(using: .utf8) {
            let txtDict = ["fp": fpData]
            let txtRecord = NetService.data(fromTXTRecord: txtDict)
            netService?.setTXTRecord(txtRecord)
        }

        netService?.delegate = self
        netService?.publish()
    }

    private func handleNewConnection(_ connection: NWConnection, capturedFingerprint: @escaping @autoclosure () -> String) {
        connection.stateUpdateHandler = { [weak self] state in
            guard let self = self else { return }
            print("TetherServer: Inbound connection state: \(state)")
            if case .ready = state {
                print("TetherServer: Inbound connection established and ready.")
                self.onNewConnection?(connection, capturedFingerprint())
            }
        }

        connection.start(queue: .main)
    }

    // NetServiceDelegate
    public func netServiceDidPublish(_ sender: NetService) {
        print("TetherServer: Bonjour service published successfully.")
    }

    public func netService(_ sender: NetService, didNotPublish errorDict: [String: NSNumber]) {
        print("TetherServer: Bonjour publish failed: \(errorDict)")
    }
}

//
//  TetherConnection.swift
//  Tether
//
//  NWConnection-based TCP + TLS client with mTLS client-certificate
//  authentication for communicating with the tetherd daemon.
//

import Foundation
import Network
import Security

// Connection state exposed to the UI.
enum ConnectionState: Sendable, Equatable {
    case disconnected
    case connecting
    case connected(isInbound: Bool)
    case failed(String)
}

// Manages a single TCP+TLS connection to a `tetherd` instance.
//
// Uses `Network.framework` (`NWConnection`) with custom TLS options that:
// - Present our self-signed client certificate for mTLS.
// - Accept any server certificate (the daemon also uses self-signed certs).
// - Capture the server's SHA-256 fingerprint during the TLS handshake.
@Observable
final class TetherConnection {
    // Current connection state.
    private(set) var state: ConnectionState = .disconnected

    // The server's TLS certificate fingerprint, captured during handshake.
    private(set) var serverFingerprint: String = ""

    private var connection: NWConnection?
    private var receiveBuffer = Data()

    // Callback invoked on the main queue for each decoded message.
    var onMessage: ((TetherMessage) -> Void)?

    // Callback invoked when the connection state changes.
    var onStateChange: ((ConnectionState) -> Void)?

    // MARK: - Public API

    // Connect to a discovered tetherd endpoint using mTLS.
    //
    // - Parameters:
    //   - endpoint: The Bonjour or direct `NWEndpoint` to connect to.
    //   - identity: The client's `SecIdentity` for mTLS authentication.
    func connect(to endpoint: NWEndpoint, identity: SecIdentity) {
        disconnect()

        let tlsOptions = NWProtocolTLS.Options()
        let secOptions = tlsOptions.securityProtocolOptions

        // Present our client certificate for mTLS
        let secIdentity = sec_identity_create(identity)!
        sec_protocol_options_set_local_identity(secOptions, secIdentity)

        // TLS 1.2 only to ensure stability with the daemon's OpenSSL configuration
        sec_protocol_options_set_min_tls_protocol_version(secOptions, .TLSv12)
        sec_protocol_options_set_max_tls_protocol_version(secOptions, .TLSv12)

        // Accept any server certificate (daemon uses self-signed)
        // and capture the server's fingerprint.
        sec_protocol_options_set_verify_block(secOptions, { [weak self] metadata, trust, completion in
            self?.captureServerFingerprint(trust: trust)
            completion(true)
        }, .main)

        let params = NWParameters(tls: tlsOptions, tcp: .init())
        params.includePeerToPeer = true

        let conn = NWConnection(to: endpoint, using: params)

        conn.stateUpdateHandler = { [weak self] nwState in
            self?.handleStateUpdate(nwState)
        }

        conn.start(queue: .main)
        self.connection = conn
        updateState(.connecting)
    }

    // Connect directly to a host:port (for manual connection).
    func connect(host: String, port: UInt16, identity: SecIdentity) {
        let endpoint = NWEndpoint.hostPort(
            host: NWEndpoint.Host(host),
            port: NWEndpoint.Port(rawValue: port)!
        )
        connect(to: endpoint, identity: identity)
    }

    // Accept a pre-established incoming connection from `TetherServer`.
    // 
    // - Parameters:
    //   - connection: The `NWConnection` already established and in `.ready` state.
    //   - fingerprint: The verified TLS fingerprint of the incoming client.
    func accept(incomingConnection conn: NWConnection, fingerprint: String) {
        disconnect()
        self.serverFingerprint = fingerprint
        self.connection = conn
        
        // Re-bind the state update handler for the accepted connection
        conn.stateUpdateHandler = { [weak self] nwState in
            self?.handleStateUpdate(nwState)
        }
        
        // It's already ready, so we just jump straight to receiving
        updateState(.connected(isInbound: true))
        startReceiving()
    }

    // Disconnect and tear down the connection.
    func disconnect() {
        connection?.cancel()
        connection = nil
        receiveBuffer = Data()
        serverFingerprint = ""
        updateState(.disconnected)
    }

    // Send a protocol message to the daemon.
    func send(_ message: TetherMessage) {
        guard let conn = connection else { return }
        
        if case .connected = state {
            do {
                let data = try TetherProtocol.encode(message)
                conn.send(content: data, completion: .contentProcessed { error in
                    if let error {
                        print("TetherConnection: Send error: \(error)")
                    }
                })
            } catch {
                print("TetherConnection: Encode error: \(error)")
            }
        }
    }

    // MARK: - Private — State

    private func updateState(_ newState: ConnectionState) {
        state = newState
        onStateChange?(newState)
    }

    private func handleStateUpdate(_ nwState: NWConnection.State) {
        switch nwState {
        case .ready:
            // Since `handleStateUpdate` is only bound for outbound connections
            // (inbound re-binds but already transitions manually in accept),
            // if this fires we assume it's outbound.
            updateState(.connected(isInbound: false))
            startReceiving()
        case .waiting(let error):
            print("TetherConnection: Waiting: \(error)")
        case .failed(let error):
            updateState(.failed(error.localizedDescription))
        case .cancelled:
            updateState(.disconnected)
        case .preparing:
            updateState(.connecting)
        default:
            break
        }
    }

    // MARK: - Private — Receive Loop

    private func startReceiving() {
        guard let conn = connection else { return }

        conn.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] content, _, isComplete, error in
            guard let self else { return }

            if let data = content, !data.isEmpty {
                self.receiveBuffer.append(data)
                let messages = TetherProtocol.decode(buffer: &self.receiveBuffer)
                for msg in messages {
                    self.onMessage?(msg)
                }
            }

            if let error {
                print("TetherConnection: Receive error: \(error)")
                self.updateState(.failed(error.localizedDescription))
                return
            }

            if isComplete {
                self.updateState(.disconnected)
                return
            }

            // Continue the receive loop
            self.startReceiving()
        }
    }

    // MARK: - Private — TLS Fingerprint

    // Extract the server's certificate and compute its SHA-256 fingerprint.
    private func captureServerFingerprint(trust: sec_trust_t) {
        let secTrust = sec_trust_copy_ref(trust).takeRetainedValue()

        if let certChain = SecTrustCopyCertificateChain(secTrust) as? [SecCertificate],
           let serverCert = certChain.first
        {
            serverFingerprint = CertificateManager.fingerprint(ofCertificate: serverCert)
        }
    }
}

//
//  TetherViewModel.swift
//  Tether
//
//  Central observable driving the entire app: discovery, pairing,
//  clipboard sync, and file transfer.
//

import Foundation
import SwiftUI
import UniformTypeIdentifiers

/// High-level app state.
enum AppConnectionState: Equatable {
    case disconnected
    case discovering
    case connecting
    case pairing
    case connected
}

/// A clipboard history entry.
struct ClipboardEntry: Identifiable {
    let id = UUID()
    let content: String
    let timestamp: Date
    let source: ClipboardSource

    enum ClipboardSource: Equatable {
        case local(String)
        case remote(String)
        
        var displayName: String {
            switch self {
            case .local(let name): return name
            case .remote(let name): return name
            }
        }
    }
}

/// Tracks an active file transfer.
struct FileTransfer: Identifiable {
    let id: String // transfer_id
    let filename: String
    let totalSize: Int64
    var bytesSent: Int64 = 0
    var isComplete = false
    var failed = false

    var progress: Double {
        guard totalSize > 0 else { return 0 }
        return Double(bytesSent) / Double(totalSize)
    }
}

/// Main app ViewModel — owns all services and coordinates their interactions.
@Observable
final class TetherViewModel {
    // MARK: - Published State

    /// Overall connection state.
    private(set) var appState: AppConnectionState = .disconnected

    /// Name of the connected device, if any.
    private(set) var connectedDeviceName: String?

    /// Clipboard history (most recent first).
    private(set) var clipboardHistory: [ClipboardEntry] = []

    /// Active file transfers.
    private(set) var activeTransfers: [FileTransfer] = []

    /// Completed file transfers.
    private(set) var completedTransfers: [FileTransfer] = []

    /// Whether the pairing sheet should be presented.
    var showPairingSheet = false

    /// Status message for the pairing flow.
    private(set) var pairingStatus: String = ""

    /// Error message to display, if any.
    var errorMessage: String?

    // MARK: - Services

    let certificateManager = CertificateManager()
    let discovery = BonjourDiscovery()
    let connection = TetherConnection()

    /// The chunk size for file transfers (48KB raw → 64KB base64).
    private let fileChunkSize = 48 * 1024

    // MARK: - Initialization

    /// Call once at app startup.
    func initialize() {
        certificateManager.initialize()
        setupConnectionHandlers()
        startDiscovery()
    }

    // MARK: - Discovery

    func startDiscovery() {
        discovery.startScanning()
        if appState == .disconnected {
            appState = .discovering
        }
    }

    func stopDiscovery() {
        discovery.stopScanning()
    }

    // MARK: - Connection

    /// Connect to a discovered host.
    func connectTo(host: DiscoveredHost) {
        guard let identity = certificateManager.getIdentity() else {
            errorMessage = "TLS identity not available. Please restart the app."
            return
        }

        appState = .connecting
        connectedDeviceName = host.name
        connection.connect(to: host.endpoint, identity: identity)
    }

    /// Connect by IP address and port.
    func connectTo(host: String, port: UInt16) {
        guard let identity = certificateManager.getIdentity() else {
            errorMessage = "TLS identity not available. Please restart the app."
            return
        }

        appState = .connecting
        connectedDeviceName = host
        connection.connect(host: host, port: port, identity: identity)
    }

    /// Disconnect from the daemon.
    func disconnect() {
        connection.disconnect()
        appState = .disconnected
        connectedDeviceName = nil
    }

    // MARK: - Clipboard

    /// Send the current iOS clipboard content to the daemon.
    func sendClipboard() {
        #if canImport(UIKit)
        guard let text = UIPasteboard.general.string, !text.isEmpty else {
            errorMessage = "Clipboard is empty."
            return
        }
        #else
        guard let text = NSPasteboard.general.string(forType: .string), !text.isEmpty else {
            errorMessage = "Clipboard is empty."
            return
        }
        #endif

        connection.send(.clipboardSet(text))

        let localName = certificateManager.localDeviceName
        let entry = ClipboardEntry(content: text, timestamp: Date(), source: .local(localName))
        clipboardHistory.insert(entry, at: 0)

        // Keep the history manageable
        if clipboardHistory.count > 50 {
            clipboardHistory = Array(clipboardHistory.prefix(50))
        }
    }

    /// Request the current clipboard from the daemon.
    func requestClipboard() {
        connection.send(.clipboardGet())
    }

    /// Copy a clipboard entry to the iOS pasteboard.
    func copyToLocalClipboard(_ text: String) {
        #if canImport(UIKit)
        UIPasteboard.general.string = text
        #else
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
        #endif
    }

    // MARK: - File Transfer

    /// Send a file to the daemon.
    func sendFile(url: URL) {
        guard url.startAccessingSecurityScopedResource() else {
            errorMessage = "Cannot access file."
            return
        }

        let transferId = UUID().uuidString
        let filename = url.lastPathComponent

        Task.detached { [weak self] in
            guard let self else { return }

            do {
                let fileData = try Data(contentsOf: url)
                let totalSize = Int64(fileData.count)

                await MainActor.run {
                    let transfer = FileTransfer(
                        id: transferId,
                        filename: filename,
                        totalSize: totalSize
                    )
                    self.activeTransfers.append(transfer)
                }

                // Send file_start
                await MainActor.run {
                    self.connection.send(.fileStart(
                        filename: filename,
                        size: totalSize,
                        transferId: transferId
                    ))
                }

                // Send chunks
                var offset = 0
                var chunkIndex = 0
                while offset < fileData.count {
                    let end = min(offset + self.fileChunkSize, fileData.count)
                    let chunk = fileData[offset..<end]
                    let base64 = chunk.base64EncodedString()

                    await MainActor.run {
                        self.connection.send(.fileChunk(
                            transferId: transferId,
                            chunkIndex: chunkIndex,
                            data: base64
                        ))

                        if let idx = self.activeTransfers.firstIndex(where: { $0.id == transferId }) {
                            self.activeTransfers[idx].bytesSent = Int64(end)
                        }
                    }

                    offset = end
                    chunkIndex += 1

                    // Small delay to avoid overwhelming the connection
                    try await Task.sleep(for: .milliseconds(10))
                }

                // Send file_end
                await MainActor.run {
                    self.connection.send(.fileEnd(transferId: transferId))
                }
            } catch {
                await MainActor.run {
                    if let idx = self.activeTransfers.firstIndex(where: { $0.id == transferId }) {
                        self.activeTransfers[idx].failed = true
                    }
                    self.errorMessage = "File transfer failed: \(error.localizedDescription)"
                }
            }

            url.stopAccessingSecurityScopedResource()
        }
    }

    // MARK: - Private — Connection Handlers

    private func setupConnectionHandlers() {
        connection.onStateChange = { [weak self] state in
            guard let self else { return }
            switch state {
            case .connected:
                self.handleConnected()
            case .disconnected:
                self.appState = .disconnected
            case .failed(let msg):
                self.appState = .disconnected
                self.errorMessage = "Connection failed: \(msg)"
            case .connecting:
                self.appState = .connecting
            }
        }

        connection.onMessage = { [weak self] message in
            self?.handleMessage(message)
        }
    }

    private func handleConnected() {
        let serverFP = connection.serverFingerprint

        if certificateManager.isHostKnown(serverFP) {
            // Already paired — go straight to connected
            appState = .connected
            connectedDeviceName = certificateManager.knownHosts[serverFP] ?? connectedDeviceName
        } else {
            // Need to pair
            appState = .pairing
            showPairingSheet = true
            pairingStatus = "Sending pairing request..."

            let deviceName = certificateManager.localDeviceName

            connection.send(.pairRequest(deviceName: deviceName))
        }
    }

    private func handleMessage(_ message: TetherMessage) {
        switch message.parsedCommand {
        case .clipboardUpdated:
            if let content = message.content {
                let sourceName = connectedDeviceName ?? "Desktop"
                let entry = ClipboardEntry(content: content, timestamp: Date(), source: .remote(sourceName))
                clipboardHistory.insert(entry, at: 0)
                if clipboardHistory.count > 50 {
                    clipboardHistory = Array(clipboardHistory.prefix(50))
                }
            }

        case .clipboardContent:
            if let content = message.content {
                let sourceName = connectedDeviceName ?? "Desktop"
                let entry = ClipboardEntry(content: content, timestamp: Date(), source: .remote(sourceName))
                clipboardHistory.insert(entry, at: 0)
            }

        case .pairPending:
            pairingStatus = "Waiting for approval on your desktop...\n\nRun: tether --accept \(certificateManager.myFingerprint)"

        case .fileStatus:
            if let transferId = message.transferId, message.status == "success" {
                if let idx = activeTransfers.firstIndex(where: { $0.id == transferId }) {
                    var transfer = activeTransfers.remove(at: idx)
                    transfer.isComplete = true
                    completedTransfers.insert(transfer, at: 0)
                }
            }

        case .error:
            if message.message == "unauthorized" {
                // The daemon rejected us — we're not paired yet.
                // This can happen if the user hasn't accepted the pairing request.
                pairingStatus = "Pairing request sent. Waiting for approval...\n\nRun: tether --accept \(certificateManager.myFingerprint)"
            } else {
                errorMessage = message.message ?? "Unknown error from daemon"
            }

        default:
            break
        }
    }

    /// Call after the daemon accepts the pairing request.
    /// The daemon will start accepting our commands, so we just need to
    /// save the server fingerprint and transition to connected state.
    func confirmPairing() {
        let serverFP = connection.serverFingerprint
        let name = connectedDeviceName ?? "Desktop"
        certificateManager.addKnownHost(fingerprint: serverFP, name: name)
        showPairingSheet = false
        appState = .connected
    }
}

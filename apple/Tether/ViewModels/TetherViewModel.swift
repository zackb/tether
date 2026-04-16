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
import UIKit

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

        var isRemote: Bool {
            if case .remote = self { return true }
            return false
        }
    }
}

/// Tracks an active file transfer.
struct FileTransfer: Identifiable {
    enum Direction {
        case outgoing
        case incoming
    }

    let id: String // transfer_id
    var filename: String
    let totalSize: Int64
    var direction: Direction
    var bytesTransferred: Int64 = 0
    var isComplete = false
    var failed = false
    var savedURL: URL?

    var progress: Double {
        guard totalSize > 0 else { return 0 }
        return Double(bytesTransferred) / Double(totalSize)
    }
}

@Observable
final class TetherViewModel {
    private static let autoSyncClipboardKey = "TetherAutoSyncClipboard"

    // MARK: - Published State

    /// Whether clipboard updates from remote devices are automatically written to the local pasteboard.
    var autoSyncClipboard: Bool = true {
        didSet {
            UserDefaults.standard.set(autoSyncClipboard, forKey: TetherViewModel.autoSyncClipboardKey)
        }
    }

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
    let server = TetherServer()

    /// The chunk size for file transfers (48KB raw → 64KB base64).
    private let fileChunkSize = 48 * 1024
    private var hasInitialized = false
    private var pendingReconnectTask: Task<Void, Never>?
    private var autoConnectingFingerprint: String?
    private var manualDisconnect = false
    private var currentScenePhase: ScenePhase = .active

    private struct IncomingTransferBuffer {
        let filename: String
        let expectedSize: Int64
        var data = Data()
    }

    private var incomingTransfers: [String: IncomingTransferBuffer] = [:]

    // MARK: - Initialization

    /// Call once at app startup.
    func initialize() {
        guard !hasInitialized else { return }
        hasInitialized = true

        if UserDefaults.standard.object(forKey: Self.autoSyncClipboardKey) != nil {
            autoSyncClipboard = UserDefaults.standard.bool(forKey: Self.autoSyncClipboardKey)
        }
        
        certificateManager.initialize()
        discovery.localFingerprint = certificateManager.myFingerprint
        
        setupConnectionHandlers()
        setupServerHandlers()
        startDiscovery()
        startServer()
        scheduleAutoReconnectAttempt()
    }

    // MARK: - Discovery

    func startDiscovery(forceRestart: Bool = false) {
        discovery.startScanning(forceRestart: forceRestart)
        if appState == .disconnected {
            appState = .discovering
        }
    }

    func stopDiscovery(clearHosts: Bool = false) {
        discovery.stopScanning(clearHosts: clearHosts)
    }

    func refreshDiscovery() {
        startDiscovery(forceRestart: true)
        scheduleAutoReconnectAttempt()
    }

    func handleScenePhase(_ phase: ScenePhase) {
        currentScenePhase = phase

        switch phase {
        case .active:
            manualDisconnect = false
            refreshDiscovery()
        case .background:
            pendingReconnectTask?.cancel()
            pendingReconnectTask = nil
            suspendForBackground()
        default:
            break
        }
    }

    // MARK: - Connection

    /// Connect to a discovered host.
    func connectTo(host: DiscoveredHost) {
        guard let identity = certificateManager.getIdentity() else {
            errorMessage = "TLS identity not available. Please restart the app."
            return
        }

        manualDisconnect = false
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

        manualDisconnect = false
        appState = .connecting
        connectedDeviceName = host
        connection.connect(host: host, port: port, identity: identity)
    }

    /// Disconnect from the daemon.
    func disconnect() {
        manualDisconnect = true
        pendingReconnectTask?.cancel()
        pendingReconnectTask = nil
        autoConnectingFingerprint = nil
        connection.disconnect()
        appState = .disconnected
        connectedDeviceName = nil
        pairingStatus = ""
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

    /// Send a file to the daemon using its URL.
    func sendFile(url: URL) {
        let isSecurityScoped = url.startAccessingSecurityScopedResource()
        let filename = url.lastPathComponent

        Task.detached { [weak self] in
            guard let self else { return }
            defer { if isSecurityScoped { url.stopAccessingSecurityScopedResource() } }

            do {
                let fileData = try Data(contentsOf: url)
                self.sendFile(data: fileData, filename: filename)
            } catch {
                await MainActor.run {
                    self.errorMessage = "File transfer failed: \(error.localizedDescription)"
                }
            }
        }
    }

    /// Send raw data to the daemon.
    func sendFile(data fileData: Data, filename: String) {
        let transferId = UUID().uuidString
        let totalSize = Int64(fileData.count)

        Task.detached { [weak self] in
            guard let self else { return }

            do {
                await MainActor.run {
                    let transfer = FileTransfer(
                        id: transferId,
                        filename: filename,
                        totalSize: totalSize,
                        direction: .outgoing
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
                            self.activeTransfers[idx].bytesTransferred = Int64(end)
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
                    self.errorMessage = "File transfer error from daemon: \(error.localizedDescription)"
                }
            }
        }
    }

    // MARK: - Private — Connection Handlers

    private func setupConnectionHandlers() {
        discovery.onHostsChanged = { [weak self] _ in
            self?.scheduleAutoReconnectAttempt()
        }

        connection.onStateChange = { [weak self] state in
            guard let self else { return }
            switch state {
            case .connected(let isInbound):
                self.pendingReconnectTask?.cancel()
                self.pendingReconnectTask = nil
                self.autoConnectingFingerprint = nil
                self.handleConnected(isInbound: isInbound)
            case .disconnected:
                self.autoConnectingFingerprint = nil
                self.appState = .disconnected
                if self.currentScenePhase == .active {
                    self.scheduleAutoReconnectAttempt()
                }
            case .failed(let msg):
                let wasAutoReconnect = self.autoConnectingFingerprint != nil
                self.autoConnectingFingerprint = nil
                self.appState = .disconnected
                if self.currentScenePhase == .active && !wasAutoReconnect {
                    self.errorMessage = "Connection failed: \(msg)"
                }
                if self.currentScenePhase == .active {
                    self.scheduleAutoReconnectAttempt()
                }
            case .connecting:
                self.appState = .connecting
            }
        }

        connection.onMessage = { [weak self] message in
            self?.handleMessage(message)
        }
    }

    private func setupServerHandlers() {
        server.onNewConnection = { [weak self] incomingConn, incomingFingerprint in
            guard let self = self else { return }
            
            // We just received an incoming connection from a peer!
            // We give it to our connection manager and start decoding.
            self.connection.accept(incomingConnection: incomingConn, fingerprint: incomingFingerprint)
            
            // Note: `onStateChange` will automatically transition us to .connected 
            // and trigger `handleConnected()` which checks if it's already paired.
        }
    }
    
    private func startServer() {
        guard let identity = certificateManager.getIdentity() else { return }
        server.start(
            identity: identity,
            localDeviceName: certificateManager.localDeviceName,
            fingerprint: certificateManager.myFingerprint
        )
    }

    private func handleConnected(isInbound: Bool = false) {
        let serverFP = connection.serverFingerprint

        if certificateManager.isHostKnown(serverFP) {
            // Already paired — go straight to connected
            appState = .connected
            connectedDeviceName = certificateManager.knownHosts[serverFP] ?? connectedDeviceName
        } else {
            // Need to pair
            if isInbound {
                // If it's an inbound connection, we wait for the client to send us a pair_request.
                // We don't send one ourselves over their established tunnel.
                appState = .pairing
                // The UI will update when we actually receive the .pairRequest command 
            } else {
                appState = .pairing
                showPairingSheet = true
                pairingStatus = "Sending pairing request..."

                let deviceName = certificateManager.localDeviceName
                connection.send(.pairRequest(deviceName: deviceName))
            }
        }
    }

    private func scheduleAutoReconnectAttempt() {
        guard shouldAutoReconnect else { return }
        guard autoConnectingFingerprint == nil else { return }

        pendingReconnectTask?.cancel()
        pendingReconnectTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(700))
            await MainActor.run {
                self?.attemptAutoReconnect()
            }
        }
    }

    private var shouldAutoReconnect: Bool {
        if manualDisconnect {
            return false
        }

        switch appState {
        case .disconnected, .discovering:
            return true
        case .connecting, .pairing, .connected:
            return false
        }
    }

    private func attemptAutoReconnect() {
        guard shouldAutoReconnect else { return }
        guard certificateManager.knownHosts.count == 1,
              let fingerprint = certificateManager.knownHosts.keys.first else { return }

        let matchingHosts = discovery.hosts.filter { $0.fingerprint == fingerprint }
        let targetHost: DiscoveredHost?

        if matchingHosts.count == 1 {
            targetHost = matchingHosts[0]
        } else if discovery.hosts.count == 1 {
            let soleHost = discovery.hosts[0]
            targetHost = soleHost.fingerprint.isEmpty || soleHost.fingerprint == fingerprint ? soleHost : nil
        } else {
            targetHost = nil
        }

        guard let targetHost else { return }

        autoConnectingFingerprint = fingerprint
        connectTo(host: targetHost)
    }

    private func suspendForBackground() {
        guard !manualDisconnect else { return }

        autoConnectingFingerprint = nil
        connection.disconnect()
        server.stop()
        appState = .disconnected
        connectedDeviceName = nil
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
                
                if autoSyncClipboard {
                    Task { @MainActor in
                        copyToLocalClipboard(content)
                    }
                }
            }

        case .clipboardContent:
            if let content = message.content {
                let sourceName = connectedDeviceName ?? "Desktop"
                let entry = ClipboardEntry(content: content, timestamp: Date(), source: .remote(sourceName))
                clipboardHistory.insert(entry, at: 0)
                
                // Manual requests ALWAYS write to the pasteboard
                Task { @MainActor in
                    copyToLocalClipboard(content)
                }
            }

        case .pairPending:
            pairingStatus = "Waiting for approval on your desktop...\n\nRun: tether --accept \(certificateManager.myFingerprint)"

        case .pairRequest:
            if let targetName = message.deviceName {
                connectedDeviceName = targetName
                appState = .pairing
                pairingStatus = "Pairing request received from \(targetName)"
                showPairingSheet = true
            }

        case .fileStatus:
            if let transferId = message.transferId, message.status == "success" {
                if let idx = activeTransfers.firstIndex(where: { $0.id == transferId }) {
                    var transfer = activeTransfers.remove(at: idx)
                    transfer.isComplete = true
                    completedTransfers.insert(transfer, at: 0)
                }
            }

        case .fileStart:
            guard let transferId = message.transferId,
                  let filename = message.filename,
                  let size = message.size else { break }

            incomingTransfers[transferId] = IncomingTransferBuffer(
                filename: filename,
                expectedSize: size
            )

            if let idx = activeTransfers.firstIndex(where: { $0.id == transferId }) {
                activeTransfers[idx].filename = filename
                activeTransfers[idx].direction = .incoming
                activeTransfers[idx].bytesTransferred = 0
            } else {
                activeTransfers.append(FileTransfer(
                    id: transferId,
                    filename: filename,
                    totalSize: size,
                    direction: .incoming
                ))
            }

        case .fileChunk:
            guard let transferId = message.transferId,
                  let data = message.data,
                  let chunkData = Data(base64Encoded: data),
                  var transfer = incomingTransfers[transferId] else { break }

            transfer.data.append(chunkData)
            incomingTransfers[transferId] = transfer

            if let idx = activeTransfers.firstIndex(where: { $0.id == transferId }) {
                activeTransfers[idx].bytesTransferred = Int64(transfer.data.count)
            }

        case .fileEnd:
            guard let transferId = message.transferId else { break }
            finalizeIncomingTransfer(transferId: transferId)

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
        
        // Let's send pair_accepted to tell the daemon we successfully paired!
        connection.send(.pairAccepted)
        
        showPairingSheet = false
        appState = .connected
    }

    private func finalizeIncomingTransfer(transferId: String) {
        guard let buffered = incomingTransfers.removeValue(forKey: transferId) else { return }

        do {
            guard Int64(buffered.data.count) == buffered.expectedSize else {
                throw CocoaError(.fileReadCorruptFile)
            }

            let destination = try incomingFileURL(for: buffered.filename)
            try buffered.data.write(to: destination, options: .atomic)

            if let idx = activeTransfers.firstIndex(where: { $0.id == transferId }) {
                var transfer = activeTransfers.remove(at: idx)
                transfer.bytesTransferred = Int64(buffered.data.count)
                transfer.isComplete = true
                transfer.savedURL = destination
                completedTransfers.insert(transfer, at: 0)
            }
        } catch {
            if let idx = activeTransfers.firstIndex(where: { $0.id == transferId }) {
                activeTransfers[idx].failed = true
            }
            errorMessage = "Failed to save incoming file: \(error.localizedDescription)"
        }
    }

    private func incomingFileURL(for filename: String) throws -> URL {
        let fm = FileManager.default
        let docs = try fm.url(
            for: .documentDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let incomingDir = docs.appendingPathComponent("Received", isDirectory: true)
        try fm.createDirectory(at: incomingDir, withIntermediateDirectories: true, attributes: nil)

        let base = URL(fileURLWithPath: filename).deletingPathExtension().lastPathComponent
        let ext = URL(fileURLWithPath: filename).pathExtension
        var candidate = incomingDir.appendingPathComponent(filename)
        var counter = 1

        while fm.fileExists(atPath: candidate.path) {
            let deduped = ext.isEmpty ? "\(base)(\(counter))" : "\(base)(\(counter)).\(ext)"
            candidate = incomingDir.appendingPathComponent(deduped)
            counter += 1
        }

        return candidate
    }
}

#if DEBUG
extension TetherViewModel {
    static var previewMock: TetherViewModel {
        let vm = TetherViewModel()
        vm.appState = .connected
        vm.connectedDeviceName = "Arch btw Linux"
        
        vm.clipboardHistory = [
            ClipboardEntry(content: "https://developer.apple.com/app-store/connect/", timestamp: Date(), source: .remote(vm.connectedDeviceName!)),
            ClipboardEntry(content: "func buildAwesomeApp() async throws -> Success", timestamp: Date().addingTimeInterval(-300), source: .local("iPhone")),
            ClipboardEntry(content: "Meeting notes:\n- Review App Store designs\n- Approve new icon\n- Push v1.0", timestamp: Date().addingTimeInterval(-3600), source: .remote(vm.connectedDeviceName!)),
            ClipboardEntry(content: "9A1B-2C3D-4E5F-6G7H", timestamp: Date().addingTimeInterval(-86400), source: .local("iPhone"))
        ]
        
        vm.activeTransfers = [
            FileTransfer(id: "1", filename: "App_Store_Assets.zip", totalSize: 24_500_000, direction: .outgoing, bytesTransferred: 18_200_000, isComplete: false)
        ]
        
        vm.completedTransfers = [
            FileTransfer(id: "2", filename: "Final_Designs.pdf", totalSize: 4_200_000, direction: .incoming, bytesTransferred: 4_200_000, isComplete: true),
            FileTransfer(id: "3", filename: "architecture_diagram.png", totalSize: 1_200_000, direction: .outgoing, bytesTransferred: 1_200_000, isComplete: true),
            FileTransfer(id: "4", filename: "auth_keys.pem", totalSize: 4_000, direction: .incoming, bytesTransferred: 4_000, isComplete: true)
        ]
        
        return vm
    }
}
#endif

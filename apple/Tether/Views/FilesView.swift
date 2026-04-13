//
//  FilesView.swift
//  Tether
//
//  File transfer interface: document picker, progress, transfer history.
//

import SwiftUI
import UniformTypeIdentifiers

struct FilesView: View {
    @Environment(TetherViewModel.self) private var viewModel
    @Environment(\.openURL) private var openURL

    @State private var showDocumentPicker = false

    var body: some View {
        NavigationStack {
            Group {
                if viewModel.appState != .connected {
                    notConnectedView
                } else {
                    connectedView
                }
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Files")
            .toolbar {
                if viewModel.appState == .connected {
                    ToolbarItem(placement: .topBarTrailing) {
                        Button {
                            showDocumentPicker = true
                        } label: {
                            Image(systemName: "plus.circle.fill")
                                .foregroundStyle(.teal)
                        }
                    }
                }
            }
            .fileImporter(
                isPresented: $showDocumentPicker,
                allowedContentTypes: [.data],
                allowsMultipleSelection: false
            ) { result in
                switch result {
                case .success(let urls):
                    if let url = urls.first {
                        viewModel.sendFile(url: url)
                    }
                case .failure(let error):
                    viewModel.errorMessage = "File selection failed: \(error.localizedDescription)"
                }
            }
        }
    }

    // MARK: - Not Connected

    private var notConnectedView: some View {
        VStack(spacing: 16) {
            Spacer()

            Image(systemName: "arrow.up.arrow.down")
                .font(.system(size: 48))
                .foregroundStyle(.tertiary)

            Text("Connect to a device to transfer files")
                .font(.subheadline)
                .foregroundStyle(.secondary)

            Spacer()
        }
        .frame(maxWidth: .infinity)
    }

    // MARK: - Connected

    private var connectedView: some View {
        ScrollView {
            VStack(spacing: 20) {
                // Send File Button
                Button {
                    showDocumentPicker = true
                } label: {
                    HStack(spacing: 14) {
                        ZStack {
                            Circle()
                                .fill(
                                    LinearGradient(
                                        colors: [.teal, .teal.opacity(0.7)],
                                        startPoint: .topLeading,
                                        endPoint: .bottomTrailing
                                    )
                                )
                                .frame(width: 48, height: 48)

                            Image(systemName: "arrow.up.doc.fill")
                                .font(.title3)
                                .foregroundStyle(.white)
                        }

                        VStack(alignment: .leading, spacing: 2) {
                            Text("Send a File")
                                .font(.body.weight(.semibold))
                                .foregroundStyle(.primary)

                            Text("Transfer to your Linux desktop")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }

                        Spacer()

                        Image(systemName: "chevron.right")
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                    .padding(16)
                    .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 16))
                }
                .buttonStyle(.plain)

                // Active Transfers
                if !viewModel.activeTransfers.isEmpty {
                    VStack(alignment: .leading, spacing: 12) {
                        Text("In Progress")
                            .font(.headline)
                            .foregroundStyle(.secondary)

                        ForEach(viewModel.activeTransfers) { transfer in
                            transferRow(transfer, isActive: true)
                        }
                    }
                }

                // Completed Transfers
                if !viewModel.completedTransfers.isEmpty {
                    VStack(alignment: .leading, spacing: 12) {
                        Text("Completed")
                            .font(.headline)
                            .foregroundStyle(.secondary)

                        ForEach(viewModel.completedTransfers) { transfer in
                            transferRow(transfer, isActive: false)
                        }
                    }
                }

                // Empty State
                if viewModel.activeTransfers.isEmpty && viewModel.completedTransfers.isEmpty {
                    VStack(spacing: 12) {
                        Image(systemName: "tray")
                            .font(.system(size: 32))
                            .foregroundStyle(.tertiary)

                        Text("No transfers yet")
                            .font(.subheadline)
                            .foregroundStyle(.tertiary)
                    }
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 40)
                }
            }
            .padding()
        }
    }

    // MARK: - Transfer Row

    @ViewBuilder
    private func transferRow(_ transfer: FileTransfer, isActive: Bool) -> some View {
        let content = HStack(spacing: 14) {
            Image(systemName: fileIcon(for: transfer.filename))
                .font(.title3)
                .foregroundStyle(transfer.direction == .incoming ? .blue : (isActive ? .teal : .secondary))
                .frame(width: 32)

            VStack(alignment: .leading, spacing: 6) {
                Text(transfer.filename)
                    .font(.subheadline.weight(.medium))
                    .lineLimit(1)

                Text(transfer.direction == .incoming ? "Incoming to iPhone" : "Outgoing to desktop")
                    .font(.caption2)
                    .foregroundStyle(.secondary)

                if isActive && !transfer.failed {
                    ProgressView(value: transfer.progress)
                        .tint(transfer.direction == .incoming ? .blue : .teal)

                    Text(formatBytes(transfer.bytesTransferred) + " / " + formatBytes(transfer.totalSize))
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                } else if transfer.failed {
                    Text("Failed")
                        .font(.caption)
                        .foregroundStyle(.red)
                } else {
                    Text(completedText(for: transfer))
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                }
            }
        }
        .padding(14)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))

        if let savedURL = transfer.savedURL, transfer.direction == .incoming, !isActive, !transfer.failed {
            Button {
                openTransfer(savedURL)
            } label: {
                content
            }
            .buttonStyle(.plain)
        } else {
            content
        }
    }

    // MARK: - Helpers

    private func fileIcon(for filename: String) -> String {
        let ext = (filename as NSString).pathExtension.lowercased()
        switch ext {
        case "png", "jpg", "jpeg", "gif", "webp", "heic":
            return "photo"
        case "mp4", "mov", "avi", "mkv":
            return "film"
        case "mp3", "wav", "flac", "aac":
            return "music.note"
        case "pdf":
            return "doc.richtext"
        case "zip", "tar", "gz", "7z":
            return "archivebox"
        case "txt", "md", "log":
            return "doc.text"
        default:
            return "doc"
        }
    }

    private func formatBytes(_ bytes: Int64) -> String {
        let formatter = ByteCountFormatter()
        formatter.countStyle = .file
        return formatter.string(fromByteCount: bytes)
    }

    private func completedText(for transfer: FileTransfer) -> String {
        if transfer.direction == .incoming {
            return "Tap to open • " + formatBytes(transfer.totalSize)
        }

        return "Delivered • " + formatBytes(transfer.totalSize)
    }

    private func openTransfer(_ url: URL) {
        openURL(url) { accepted in
            if !accepted {
                viewModel.errorMessage = "Unable to open \(url.lastPathComponent)."
            }
        }
    }
}

#Preview {
    FilesView()
        .environment(TetherViewModel())
        .preferredColorScheme(.dark)
}

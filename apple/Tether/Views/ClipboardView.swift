//
//  ClipboardView.swift
//  Tether
//
//  Clipboard sync interface: send/receive buttons, clipboard history.
//

import SwiftUI

struct ClipboardView: View {
    @Environment(TetherViewModel.self) private var viewModel

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
            .navigationTitle("Clipboard")
        }
    }

    // MARK: - Not Connected

    private var notConnectedView: some View {
        VStack(spacing: 16) {
            Spacer()

            Image(systemName: "clipboard")
                .font(.system(size: 48))
                .foregroundStyle(.tertiary)

            Text("Connect to a device to sync clipboard")
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
                // Action Buttons
                HStack(spacing: 12) {
                    clipboardActionButton(
                        title: "Send to Desktop",
                        subtitle: "Copy iPhone clipboard",
                        icon: "arrow.up.circle.fill",
                        color: .teal
                    ) {
                        viewModel.sendClipboard()
                    }

                    clipboardActionButton(
                        title: "Get from Desktop",
                        subtitle: "Fetch desktop clipboard",
                        icon: "arrow.down.circle.fill",
                        color: .indigo
                    ) {
                        viewModel.requestClipboard()
                    }
                }
                .padding(.horizontal)
                .padding(.top, 4)

                // History
                VStack(alignment: .leading, spacing: 12) {
                    Text("History")
                        .font(.headline)
                        .foregroundStyle(.secondary)
                        .padding(.horizontal)

                    if viewModel.clipboardHistory.isEmpty {
                        VStack(spacing: 12) {
                            Image(systemName: "clock.arrow.circlepath")
                                .font(.system(size: 32))
                                .foregroundStyle(.tertiary)

                            Text("No clipboard activity yet")
                                .font(.subheadline)
                                .foregroundStyle(.tertiary)
                        }
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 40)
                    } else {
                        LazyVStack(spacing: 8) {
                            ForEach(viewModel.clipboardHistory) { entry in
                                clipboardEntryRow(entry)
                            }
                        }
                        .padding(.horizontal)
                    }
                }
            }
            .padding(.vertical)
        }
    }

    // MARK: - Components

    private func clipboardActionButton(
        title: String,
        subtitle: String,
        icon: String,
        color: Color,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(alignment: .leading, spacing: 8) {
                Image(systemName: icon)
                    .font(.title)
                    .foregroundStyle(color)

                VStack(alignment: .leading, spacing: 2) {
                    Text(title)
                        .font(.subheadline.weight(.semibold))
                        .foregroundStyle(.primary)

                    Text(subtitle)
                        .font(.caption2)
                        .foregroundStyle(.tertiary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(16)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
    }

    private func clipboardEntryRow(_ entry: ClipboardEntry) -> some View {
        Button {
            viewModel.copyToLocalClipboard(entry.content)
        } label: {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: entry.source == .remote ? "desktopcomputer" : "iphone")
                    .font(.subheadline)
                    .foregroundStyle(entry.source == .remote ? .indigo : .teal)
                    .frame(width: 24)
                    .padding(.top, 2)

                VStack(alignment: .leading, spacing: 4) {
                    Text(entry.content)
                        .font(.subheadline)
                        .foregroundStyle(.primary)
                        .lineLimit(3)

                    HStack(spacing: 6) {
                        Text(entry.source.displayName)
                            .font(.caption2.weight(.medium))
                            .foregroundStyle(entry.source == .remote ? .indigo : .teal)

                        Text("·")
                            .foregroundStyle(.quaternary)

                        Text(entry.timestamp, style: .relative)
                            .font(.caption2)
                            .foregroundStyle(.quaternary)
                    }
                }

                Spacer()

                Image(systemName: "doc.on.doc")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
            .padding(14)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 12))
        }
        .buttonStyle(.plain)
    }
}

#Preview {
    ClipboardView()
        .environment(TetherViewModel())
        .preferredColorScheme(.dark)
}

//
//  DashboardView.swift
//  Tether
//
//  Main dashboard: connection status, discovered hosts, quick actions.
//

import SwiftUI
import TetherFramework

struct DashboardView: View {
    @Environment(TetherViewModel.self) private var viewModel

    @State private var manualHost = ""
    @State private var manualPort = "5134"
    @State private var showManualConnect = false
    @State private var bluetoothHosts: [DiscoveredHost] = []
    @State private var bluetoothSearching = false
    @State private var bluetoothSearched = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 20) {
                    // Status Card
                    statusCard

                    // Quick Actions (when connected)
                    if viewModel.appState == .connected {
                        quickActions
                    }

                    // Discovered Hosts (when not connected)
                    if viewModel.appState != .connected {
                        discoveredHostsSection
                    }
                }
                .padding()
            }
            .background(Color(.systemGroupedBackground))
            .navigationTitle("Tether")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    if viewModel.appState == .connected {
                        Button {
                            viewModel.disconnect()
                        } label: {
                            Image(systemName: "xmark.circle.fill")
                                .foregroundStyle(.secondary)
                        }
                        .accessibilityLabel("Disconnect")
                    } else {
                        Button {
                            showManualConnect.toggle()
                        } label: {
                            Image(systemName: "network")
                                .foregroundStyle(.teal)
                        }
                        .accessibilityLabel("Connect Manually")
                    }
                }
            }
            .sheet(isPresented: $showManualConnect) {
                manualConnectSheet
                    .onAppear(perform: prefillManualConnect)
            }
        }
    }

    // MARK: - Status Card

    private var statusCard: some View {
        VStack(spacing: 16) {
            // Large icon
            ZStack {
                Circle()
                    .fill(statusGradient)
                    .frame(width: 80, height: 80)

                Image(systemName: statusIcon)
                    .font(.system(size: 32, weight: .medium))
                    .foregroundStyle(.white)
            }
            .shadow(color: statusAccentColor.opacity(0.4), radius: 12, y: 4)
            .accessibilityHidden(true)

            VStack(spacing: 6) {
                if let deviceName = viewModel.connectedDeviceName,
                   viewModel.appState == .connected
                {
                    Text(deviceName)
                        .font(.title2.weight(.semibold))

                    ConnectionStatusView(state: viewModel.appState)
                } else {
                    Text("No Connection")
                        .font(.title2.weight(.semibold))

                    ConnectionStatusView(state: viewModel.appState)
                }
            }

            // Fingerprint
            if !viewModel.certificateManager.myFingerprint.isEmpty {
                VStack(spacing: 4) {
                    Text("Your Fingerprint")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .textCase(.uppercase)

                    Text(formatFingerprint(viewModel.certificateManager.myFingerprint))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .textSelection(.enabled)
                }
                .padding(.top, 4)
            }
        }
        .padding(24)
        .frame(maxWidth: .infinity)
        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 20))
    }

    private var statusIcon: String {
        switch viewModel.appState {
        case .connected: "link"
        case .connecting, .pairing: "arrow.triangle.2.circlepath"
        case .discovering: "magnifyingglass"
        case .disconnected: "link.badge.plus"
        }
    }

    private var statusAccentColor: Color {
        switch viewModel.appState {
        case .connected: .green
        case .connecting, .pairing, .discovering: .orange
        case .disconnected: .teal
        }
    }

    private var statusGradient: LinearGradient {
        LinearGradient(
            colors: [statusAccentColor, statusAccentColor.opacity(0.7)],
            startPoint: .topLeading,
            endPoint: .bottomTrailing
        )
    }

    // MARK: - Quick Actions

    private var quickActions: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Quick Actions")
                .font(.headline)
                .foregroundStyle(.secondary)

            HStack(spacing: 12) {
                actionButton(
                    title: "Send Clipboard",
                    icon: "doc.on.clipboard.fill",
                    color: .teal
                ) {
                    viewModel.sendClipboard()
                }

                actionButton(
                    title: "Get Clipboard",
                    icon: "arrow.down.doc.fill",
                    color: .indigo
                ) {
                    viewModel.requestClipboard()
                }
            }
        }
    }

    private func actionButton(
        title: LocalizedStringKey,
        icon: String,
        color: Color,
        action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: 10) {
                Image(systemName: icon)
                    .font(.title2)
                    .foregroundStyle(color)
                    .accessibilityHidden(true)

                Text(title)
                    .font(.caption.weight(.medium))
                    .foregroundStyle(.primary)
            }
            .frame(maxWidth: .infinity)
            .padding(.vertical, 16)
            .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
    }

    // MARK: - Discovered Hosts

    private var discoveredHostsSection: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Discovered Devices")
                    .font(.headline)
                    .foregroundStyle(.secondary)

                Spacer()

                if viewModel.discovery.isScanning {
                    ProgressView()
                        .controlSize(.small)
                }
            }

            if viewModel.discovery.hosts.isEmpty {
                VStack(spacing: 12) {
                    Image(systemName: "radar")
                        .font(.system(size: 36))
                        .foregroundStyle(.tertiary)
                        .accessibilityHidden(true)

                    Text("Searching for tetherd on your network…")
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)

                    Text("Make sure tetherd is running on your Linux machine.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                .frame(maxWidth: .infinity)
                .padding(.vertical, 32)
            } else {
                ForEach(viewModel.discovery.hosts) { host in
                    Button {
                        viewModel.connectTo(host: host)
                    } label: {
                        HStack(spacing: 14) {
                            Image(systemName: "desktopcomputer")
                                .font(.title3)
                                .foregroundStyle(.teal)
                                .frame(width: 36)
                                .accessibilityHidden(true)

                            VStack(alignment: .leading, spacing: 2) {
                                Text(host.name)
                                    .font(.body.weight(.medium))
                                    .foregroundStyle(.primary)

                                if !host.fingerprint.isEmpty {
                                    Text(formatFingerprint(String(host.fingerprint.prefix(16))) + "…")
                                        .font(.system(.caption2, design: .monospaced))
                                        .foregroundStyle(.secondary)
                                }
                            }

                            Spacer()

                            if viewModel.certificateManager.isHostKnown(host.fingerprint) {
                                Image(systemName: "checkmark.shield.fill")
                                    .foregroundStyle(.green)
                                    .font(.subheadline)
                                    .accessibilityLabel("Paired")
                            }

                            Image(systemName: "chevron.right")
                                .font(.caption)
                                .foregroundStyle(.tertiary)
                                .accessibilityHidden(true)
                        }
                        .padding(14)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 14))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    // MARK: - Manual Connect Sheet

    private var manualConnectSheet: some View {
        NavigationStack {
            Form {
                Section("Host") {
                    TextField("Host or IP", text: $manualHost)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .autocorrectionDisabled()

                    TextField("Port", text: $manualPort)
                        .keyboardType(.numberPad)
                }

                Section {
                    Button("Connect") {
                        let port = UInt16(manualPort) ?? 5134
                        viewModel.connectTo(host: manualHost, port: port)
                        showManualConnect = false
                    }
                    .disabled(manualHost.isEmpty)
                }

                bluetoothSection
            }
            .navigationTitle("Manual Connection")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { showManualConnect = false }
                }
            }
        }
        .presentationDetents([.medium])
    }

    // For networks that block mDNS: the computer reports its address over the Bluetooth bond.
    private var bluetoothSection: some View {
        Section {
            Button(action: findOverBluetooth) {
                HStack {
                    Label("Find over Bluetooth", systemImage: "antenna.radiowaves.left.and.right")
                    Spacer()
                    if bluetoothSearching {
                        ProgressView()
                    }
                }
            }
            .disabled(bluetoothSearching)

            ForEach(bluetoothHosts) { host in
                Button(host.name) {
                    viewModel.connectTo(host: host)
                    showManualConnect = false
                }
            }
        } footer: {
            if bluetoothSearched && !bluetoothSearching && bluetoothHosts.isEmpty {
                Text("No computer found. Pair this iPhone with your computer in Bluetooth settings and make sure tetherd is running.")
            }
        }
    }

    // MARK: - Helpers

    private func findOverBluetooth() {
        bluetoothSearching = true
        Task {
            let hosts = await viewModel.bluetooth.lookup()
            bluetoothSearching = false
            bluetoothSearched = true
            bluetoothHosts = hosts
            if hosts.count == 1 {
                viewModel.connectTo(host: hosts[0])
                showManualConnect = false
            }
        }
    }

    // Seed the sheet with the address that last worked, so recovering a connection
    // discovery cannot make is a tap rather than a retype.
    private func prefillManualConnect() {
        bluetoothHosts = []
        bluetoothSearched = false
        guard manualHost.isEmpty, let endpoint = ShareSender.lastEndpoint() else { return }
        manualHost = endpoint.host
        manualPort = String(endpoint.port)
    }

    private func formatFingerprint(_ fp: String) -> String {
        var result = ""
        for (i, char) in fp.enumerated() {
            if i > 0 && i % 2 == 0 { result += ":" }
            result.append(char)
        }
        return result
    }
}

#Preview {
    DashboardView()
        .environment(TetherViewModel())
        .preferredColorScheme(.dark)
}

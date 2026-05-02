//
//  ConnectionStatusView.swift
//  Tether
//
//  Reusable animated connection status indicator.
//

import SwiftUI
import TetherFramework

struct ConnectionStatusView: View {
    let state: AppConnectionState

    @State private var isPulsing = false

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(dotColor)
                .frame(width: 10, height: 10)
                .scaleEffect(isPulsing ? 1.3 : 1.0)
                .opacity(isPulsing ? 0.6 : 1.0)
                .animation(
                    isAnimating
                        ? .easeInOut(duration: 0.8).repeatForever(autoreverses: true)
                        : .default,
                    value: isPulsing
                )

            Text(statusText)
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
        .onChange(of: state, initial: true) {
            isPulsing = isAnimating
        }
    }

    private var dotColor: Color {
        switch state {
        case .connected: .green
        case .connecting, .pairing, .discovering: .orange
        case .disconnected: .red
        }
    }

    private var statusText: String {
        switch state {
        case .connected: "Connected"
        case .connecting: "Connecting…"
        case .pairing: "Pairing…"
        case .discovering: "Searching…"
        case .disconnected: "Disconnected"
        }
    }

    private var isAnimating: Bool {
        switch state {
        case .connecting, .pairing, .discovering: true
        default: false
        }
    }
}

#Preview {
    VStack(spacing: 20) {
        ConnectionStatusView(state: .connected)
        ConnectionStatusView(state: .connecting)
        ConnectionStatusView(state: .disconnected)
        ConnectionStatusView(state: .discovering)
        ConnectionStatusView(state: .pairing)
    }
    .padding()
    .preferredColorScheme(.dark)
}

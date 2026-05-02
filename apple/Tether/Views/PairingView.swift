//
//  PairingView.swift
//  Tether
//
//  Sheet presented during the device pairing flow.
//

import SwiftUI
import TetherFramework

struct PairingView: View {
    @Environment(TetherViewModel.self) private var viewModel

    @State private var rotationAngle: Double = 0

    var body: some View {
        NavigationStack {
            VStack(spacing: 32) {
                Spacer()

                // Animated pairing icon
                ZStack {
                    // Outer rotating ring
                    Circle()
                        .strokeBorder(
                            AngularGradient(
                                colors: [.teal, .teal.opacity(0.2), .teal],
                                center: .center
                            ),
                            lineWidth: 3
                        )
                        .frame(width: 100, height: 100)
                        .rotationEffect(.degrees(rotationAngle))

                    // Inner icon
                    Image(systemName: "lock.shield")
                        .font(.system(size: 36, weight: .medium))
                        .foregroundStyle(.teal)
                }
                .onAppear {
                    withAnimation(.linear(duration: 3).repeatForever(autoreverses: false)) {
                        rotationAngle = 360
                    }
                }

                // Status Text
                VStack(spacing: 12) {
                    Text("Device Pairing")
                        .font(.title2.weight(.bold))

                    Text(viewModel.pairingStatus.isEmpty
                         ? "Initiating secure pairing…"
                         : viewModel.pairingStatus)
                        .font(.subheadline)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 32)
                }

                // Fingerprint Display
                VStack(spacing: 8) {
                    Text("Your Device Fingerprint")
                        .font(.caption)
                        .foregroundStyle(.tertiary)
                        .textCase(.uppercase)

                    Text(formatFingerprint(viewModel.certificateManager.myFingerprint))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 10)
                        .background(.ultraThinMaterial, in: RoundedRectangle(cornerRadius: 8))
                        .textSelection(.enabled)
                }

                Spacer()

                // Actions
                VStack(spacing: 12) {
                    Button {
                        viewModel.confirmPairing()
                    } label: {
                        Text("I've Approved on Desktop")
                            .font(.body.weight(.semibold))
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 14)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.teal)

                    Button {
                        viewModel.disconnect()
                        viewModel.showPairingSheet = false
                    } label: {
                        Text("Cancel")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                }
                .padding(.horizontal, 24)
                .padding(.bottom, 16)
            }
            .padding()
            .navigationBarTitleDisplayMode(.inline)
        }
        .presentationDetents([.large])
        .presentationDragIndicator(.visible)
    }

    // MARK: - Helpers

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
    PairingView()
        .environment(TetherViewModel())
        .preferredColorScheme(.dark)
}

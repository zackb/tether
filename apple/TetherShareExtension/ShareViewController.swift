//
//  ShareViewController.swift
//  TetherShareExtension
//
//  The Share Extension principal class.
//
//  Parses the incoming NSExtensionItem to determine content type, then presents
//  a SwiftUI sheet with only the routes applicable to that content:
//
//    • Text  → 📋 Send to Clipboard  |  🔑 Send as OTP
//    • Image → 📁 Send as File
//    • File  → 📁 Send as File
//

import SwiftUI
import UIKit
import UniformTypeIdentifiers
import TetherFramework

// MARK: - ShareViewController (UIViewController bridge)

final class ShareViewController: UIViewController {

    override func viewDidLoad() {
        super.viewDidLoad()
        view.backgroundColor = UIColor.systemBackground.withAlphaComponent(0)
    }

    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        loadPayload { [weak self] payload in
            guard let self else { return }
            let hostingVC = UIHostingController(
                rootView: ShareSheetView(
                    payload: payload,
                    onComplete: { [weak self] in self?.completeRequest() },
                    onCancel: { [weak self] in self?.cancelRequest() }
                )
                .preferredColorScheme(.dark)
            )
            hostingVC.modalPresentationStyle = .pageSheet
            if let sheet = hostingVC.sheetPresentationController {
                sheet.detents = [.medium()]
                sheet.prefersGrabberVisible = true
            }
            self.present(hostingVC, animated: true)
        }
    }

    // MARK: - Private

    private func loadPayload(completion: @escaping (IncomingPayload) -> Void) {
        guard let item = extensionContext?.inputItems.first as? NSExtensionItem,
              let attachments = item.attachments, !attachments.isEmpty else {
            completion(.unsupported)
            return
        }

        let provider = attachments[0]

        // 1. Explicit File URL (Files app, high-quality Photo Library shares)
        if provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                DispatchQueue.main.async {
                    if let url = item as? URL {
                        let isSecured = url.startAccessingSecurityScopedResource()
                        defer { if isSecured { url.stopAccessingSecurityScopedResource() } }
                        let filename = url.lastPathComponent
                        let data = (try? Data(contentsOf: url)) ?? Data()
                        completion(.file(data, filename: filename))
                    } else {
                        completion(.unsupported)
                    }
                }
            }
            return
        }

        // 2. General URL (Could be web link or disguised file URL)
        if provider.hasItemConformingToTypeIdentifier(UTType.url.identifier) {
            provider.loadItem(forTypeIdentifier: UTType.url.identifier, options: nil) { item, _ in
                DispatchQueue.main.async {
                    if let url = item as? URL, url.isFileURL {
                        let isSecured = url.startAccessingSecurityScopedResource()
                        defer { if isSecured { url.stopAccessingSecurityScopedResource() } }
                        let filename = url.lastPathComponent
                        let data = (try? Data(contentsOf: url)) ?? Data()
                        completion(.file(data, filename: filename))
                    } else {
                        let text = (item as? URL)?.absoluteString ?? (item as? String) ?? ""
                        completion(.text(text))
                    }
                }
            }
            return
        }

        // 3. Plain Text (Must be checked after URLs so we don't accidentally treat links as raw strings too early, though order here is flexible)
        if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
            provider.loadItem(forTypeIdentifier: UTType.plainText.identifier, options: nil) { item, _ in
                DispatchQueue.main.async {
                    let text = (item as? String) ?? ""
                    completion(.text(text))
                }
            }
            return
        }

        // 4. Image fallback (If it didn't conform to fileURL, like in-memory UIImages)
        if provider.hasItemConformingToTypeIdentifier(UTType.image.identifier) {
            provider.loadItem(forTypeIdentifier: UTType.image.identifier, options: nil) { item, _ in
                DispatchQueue.main.async {
                    if let image = item as? UIImage, let data = image.jpegData(compressionQuality: 0.9) {
                        completion(.file(data, filename: "image.jpg"))
                    } else if let url = item as? URL {
                        let isSecured = url.startAccessingSecurityScopedResource()
                        defer { if isSecured { url.stopAccessingSecurityScopedResource() } }
                        let filename = url.lastPathComponent
                        let data = (try? Data(contentsOf: url)) ?? Data()
                        completion(.file(data, filename: filename))
                    } else {
                        completion(.unsupported)
                    }
                }
            }
            return
        }

        // 5. Generic Data (Other file representations)
        if provider.hasItemConformingToTypeIdentifier(UTType.data.identifier) {
            provider.loadItem(forTypeIdentifier: UTType.data.identifier, options: nil) { item, _ in
                DispatchQueue.main.async {
                    if let url = item as? URL {
                        let isSecured = url.startAccessingSecurityScopedResource()
                        defer { if isSecured { url.stopAccessingSecurityScopedResource() } }
                        let filename = url.lastPathComponent
                        let data = (try? Data(contentsOf: url)) ?? Data()
                        completion(.file(data, filename: filename))
                    } else if let data = item as? Data {
                        completion(.file(data, filename: "shared_file.bin"))
                    } else {
                        completion(.unsupported)
                    }
                }
            }
            return
        }

        completion(.unsupported)
    }

    private func completeRequest() {
        extensionContext?.completeRequest(returningItems: nil, completionHandler: nil)
    }

    private func cancelRequest() {
        extensionContext?.cancelRequest(withError: NSError(
            domain: "net.jeedup.TetherShareExtension",
            code: NSUserCancelledError
        ))
    }
}

// MARK: - Incoming Payload (internal to extension)

enum IncomingPayload {
    case text(String)
    case file(Data, filename: String)
    case unsupported
}

// MARK: - ShareSheetView

private struct ShareSheetView: View {
    let payload: IncomingPayload
    let onComplete: () -> Void
    let onCancel: () -> Void

    @State private var isSending = false
    @State private var resultMessage: String?
    @State private var didFail = false

    var body: some View {
        NavigationStack {
            ZStack {
                // Dark gradient background
                LinearGradient(
                    colors: [
                        Color(hue: 0.6, saturation: 0.3, brightness: 0.12),
                        Color(hue: 0.62, saturation: 0.4, brightness: 0.08),
                    ],
                    startPoint: .topLeading,
                    endPoint: .bottomTrailing
                )
                .ignoresSafeArea()

                if isSending {
                    sendingView
                } else if let result = resultMessage {
                    resultView(message: result, failed: didFail)
                } else {
                    actionPickerView
                }
            }
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .navigationBarLeading) {
                    Button("Cancel") { onCancel() }
                        .foregroundStyle(.secondary)
                }
                ToolbarItem(placement: .principal) {
                    HStack(spacing: 6) {
                        Image(systemName: "bolt.fill")
                            .foregroundStyle(.cyan)
                        Text("Tether")
                            .font(.headline)
                            .foregroundStyle(.white)
                    }
                }
            }
        }
    }

    // MARK: Action Picker

    @ViewBuilder
    private var actionPickerView: some View {
        VStack(spacing: 20) {
            // Content preview
            contentPreview
                .padding(.top, 8)

            Divider().background(.white.opacity(0.15))

            // Route buttons — only show relevant ones
            VStack(spacing: 12) {
                switch payload {
                case .text(let text):
                    ActionButton(
                        icon: "clipboard",
                        label: "Send to Clipboard",
                        subtitle: "Sets your desktop clipboard",
                        color: .cyan
                    ) {
                        perform(.clipboard(text))
                    }
                    ActionButton(
                        icon: "key.fill",
                        label: "Send as OTP",
                        subtitle: "Stores in the Tether OTP vault",
                        color: Color(hue: 0.15, saturation: 0.8, brightness: 0.9)
                    ) {
                        perform(.otp(text, source: "iPhone Share"))
                    }

                case .file(let data, let filename):
                    ActionButton(
                        icon: "arrow.up.doc.fill",
                        label: "Send as File",
                        subtitle: filename,
                        color: Color(hue: 0.75, saturation: 0.7, brightness: 0.9)
                    ) {
                        perform(.file(data, filename: filename))
                    }

                case .unsupported:
                    Text("This content type isn't supported by Tether.")
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                        .padding()
                }
            }
            .padding(.horizontal)

            Spacer()
        }
    }

    // MARK: Content Preview

    @ViewBuilder
    private var contentPreview: some View {
        switch payload {
        case .text(let text):
            VStack(alignment: .leading, spacing: 4) {
                Label("Text", systemImage: "text.quote")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(text)
                    .lineLimit(3)
                    .font(.body)
                    .foregroundStyle(.white)
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.white.opacity(0.07))
                    .clipShape(RoundedRectangle(cornerRadius: 10))
            }
            .padding(.horizontal)

        case .file(_, let filename):
            VStack(spacing: 6) {
                Image(systemName: "doc.fill")
                    .font(.largeTitle)
                    .foregroundStyle(.purple)
                Text(filename)
                    .font(.subheadline)
                    .foregroundStyle(.white)
            }
            .padding()

        case .unsupported:
            EmptyView()
        }
    }

    // MARK: Sending / Result

    private var sendingView: some View {
        VStack(spacing: 16) {
            ProgressView()
                .tint(.cyan)
                .scaleEffect(1.4)
            Text("Connecting to Tether...")
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }

    private func resultView(message: String, failed: Bool) -> some View {
        VStack(spacing: 16) {
            Image(systemName: failed ? "xmark.circle.fill" : "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(failed ? .red : .green)
                .symbolEffect(.bounce)
            Text(message)
                .multilineTextAlignment(.center)
                .foregroundStyle(.white)
                .padding(.horizontal)
            if failed {
                Button("Close") { onCancel() }
                    .buttonStyle(.borderedProminent)
                    .tint(.secondary)
            }
        }
        .onAppear {
            if !failed {
                DispatchQueue.main.asyncAfter(deadline: .now() + 1.4) {
                    onComplete()
                }
            }
        }
    }

    // MARK: Send Logic

    private func perform(_ sharePayload: SharePayload) {
        isSending = true
        Task {
            let result = await ShareSender.send(sharePayload)
            await MainActor.run {
                isSending = false
                switch result {
                case .success:
                    resultMessage = successMessage(for: sharePayload)
                    didFail = false
                case .failure(let error):
                    resultMessage = error.localizedDescription
                    didFail = true
                }
            }
        }
    }

    private func successMessage(for payload: SharePayload) -> String {
        switch payload {
        case .clipboard: return "Sent to clipboard ✓"
        case .otp: return "OTP stored in vault ✓"
        case .file(_, let name): return "\(name) sent ✓"
        }
    }
}

// MARK: - ActionButton

private struct ActionButton: View {
    let icon: String
    let label: String
    let subtitle: String
    let color: Color
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 14) {
                ZStack {
                    RoundedRectangle(cornerRadius: 10)
                        .fill(color.opacity(0.2))
                        .frame(width: 44, height: 44)
                    Image(systemName: icon)
                        .font(.system(size: 20, weight: .semibold))
                        .foregroundStyle(color)
                }
                VStack(alignment: .leading, spacing: 2) {
                    Text(label)
                        .font(.body.weight(.semibold))
                        .foregroundStyle(.white)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.tertiary)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .background(.white.opacity(0.06))
            .clipShape(RoundedRectangle(cornerRadius: 14))
            .overlay(
                RoundedRectangle(cornerRadius: 14)
                    .stroke(.white.opacity(0.08), lineWidth: 1)
            )
        }
        .buttonStyle(.plain)
    }
}

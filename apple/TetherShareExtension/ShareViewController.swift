//
//  ShareViewController.swift
//  TetherShareExtension
//
//  The Share Extension principal class.
//
//  Parses the incoming NSExtensionItem(s) to determine content, then presents
//  a SwiftUI sheet with only the routes applicable to that content:
//
//    • Text (single item)  → 📋 Send to Clipboard  |  🔑 Send as OTP
//    • Image (single item) → 📋 Copy to Clipboard  |  📁 Send as File
//    • Video/Contact/Audio/File (single item) → 📁 Send as File
//    • Multiple items (Photos multi-select, etc.)   → 📁 Send N Files
//
//  Every attachment on every input item is resolved (not just the first),
//  so multi-select shares from Photos/Files aren't silently truncated.
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
            let appearance = AppearancePreference.current
            let hostingVC = UIHostingController(
                rootView: ShareSheetView(
                    payload: payload,
                    onComplete: { [weak self] in self?.completeRequest() },
                    onCancel: { [weak self] in self?.cancelRequest() }
                )
                .preferredColorScheme(appearance.colorScheme)
            )
            hostingVC.overrideUserInterfaceStyle = switch appearance {
            case .system: .unspecified
            case .light: .light
            case .dark: .dark
            }
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
        let providers: [NSItemProvider] = (extensionContext?.inputItems as? [NSExtensionItem] ?? [])
            .flatMap { $0.attachments ?? [] }

        guard !providers.isEmpty else {
            completion(.unsupported)
            return
        }

        Task {
            var items: [SharedItem] = []
            for provider in providers {
                if let item = await Self.resolveItem(from: provider) {
                    items.append(item)
                }
            }

            await MainActor.run {
                switch items.count {
                case 0:
                    completion(.unsupported)
                case 1:
                    completion(.single(items[0]))
                default:
                    completion(.multiple(items))
                }
            }
        }
    }

    /// Resolves a single `NSItemProvider` into a `SharedItem`, checking
    /// content types in priority order. Returns `nil` if nothing usable
    /// could be extracted.
    private static func resolveItem(from provider: NSItemProvider) async -> SharedItem? {
        // 1. Explicit File URL (Files app, high-quality Photo Library shares)
        if provider.hasItemConformingToTypeIdentifier(UTType.fileURL.identifier) {
            return await loadFileBackedItem(provider, typeIdentifier: UTType.fileURL.identifier)
        }

        // 2. General URL (Could be web link or disguised file URL)
        if provider.hasItemConformingToTypeIdentifier(UTType.url.identifier) {
            return await withCheckedContinuation { continuation in
                provider.loadItem(forTypeIdentifier: UTType.url.identifier, options: nil) { item, _ in
                    if let url = item as? URL, url.isFileURL {
                        continuation.resume(returning: readFile(at: url))
                    } else {
                        let text = (item as? URL)?.absoluteString ?? (item as? String) ?? ""
                        continuation.resume(returning: text.isEmpty ? nil : .text(text))
                    }
                }
            }
        }

        // 3. Plain Text (Must be checked after URLs so we don't accidentally treat links as raw strings too early, though order here is flexible)
        if provider.hasItemConformingToTypeIdentifier(UTType.plainText.identifier) {
            return await withCheckedContinuation { continuation in
                provider.loadItem(forTypeIdentifier: UTType.plainText.identifier, options: nil) { item, _ in
                    let text = (item as? String) ?? ""
                    continuation.resume(returning: text.isEmpty ? nil : .text(text))
                }
            }
        }

        // 4. Image (in-memory UIImage or URL-backed)
        if provider.hasItemConformingToTypeIdentifier(UTType.image.identifier) {
            // prefer the original encoding (heic)
            if let original = await loadOriginalImage(provider) {
                return original
            }
            // Instant Markup (screenshot editor) hands back raw encoded bytes;
            // name them after the provider's first image type.
            let ext = provider.registeredTypeIdentifiers
                .compactMap { UTType($0) }
                .first { $0.conforms(to: .image) }?
                .preferredFilenameExtension ?? "png"
            return await withCheckedContinuation { continuation in
                provider.loadItem(forTypeIdentifier: UTType.image.identifier, options: nil) { item, _ in
                    if let image = item as? UIImage, let data = image.jpegData(compressionQuality: 0.9) {
                        continuation.resume(returning: .file(data, filename: "image.jpg"))
                    } else if let url = item as? URL {
                        continuation.resume(returning: readFile(at: url, defaultFilename: "image.jpg"))
                    } else if let data = item as? Data {
                        continuation.resume(returning: .file(data, filename: "image.\(ext)"))
                    } else {
                        continuation.resume(returning: nil)
                    }
                }
            }
        }

        // 5. Video/Movie
        if provider.hasItemConformingToTypeIdentifier(UTType.movie.identifier) {
            return await loadFileBackedItem(provider, typeIdentifier: UTType.movie.identifier, defaultFilename: "video.mov")
        }

        // 6. Contact (vCard) — rides the generic file path; lands as a plain
        // .vcf file on the Linux side, same as any other file.
        if provider.hasItemConformingToTypeIdentifier(UTType.vCard.identifier) {
            return await loadFileBackedItem(provider, typeIdentifier: UTType.vCard.identifier, defaultFilename: "Contact.vcf")
        }

        // 7. Audio
        if provider.hasItemConformingToTypeIdentifier(UTType.audio.identifier) {
            return await loadFileBackedItem(provider, typeIdentifier: UTType.audio.identifier, defaultFilename: "audio.m4a")
        }

        // 8. Generic Data (catch-all for any other file representation)
        if provider.hasItemConformingToTypeIdentifier(UTType.data.identifier) {
            return await loadFileBackedItem(provider, typeIdentifier: UTType.data.identifier, defaultFilename: "shared_file.bin")
        }

        // 9. Object-only image (e.g. "com.apple.uikit.image" from the screenshot
        // editor), which conforms to none of the UTTypes above.
        if provider.canLoadObject(ofClass: UIImage.self) {
            return await withCheckedContinuation { continuation in
                _ = provider.loadObject(ofClass: UIImage.self) { object, _ in
                    let data = (object as? UIImage)?.pngData()
                    continuation.resume(returning: data.map { .file($0, filename: "image.png") })
                }
            }
        }

        NSLog("Tether share: unhandled types %@", provider.registeredTypeIdentifiers as NSArray)
        return nil
    }

    /// Loads an item expected to be either a `URL` (file-backed) or raw
    /// `Data`, wrapping it as `.file`. Used for any URL-or-Data-representable
    /// type identifier (movie, vCard, audio, generic data, etc.).
    private static func loadFileBackedItem(
        _ provider: NSItemProvider,
        typeIdentifier: String,
        defaultFilename: String? = nil
    ) async -> SharedItem? {
        await withCheckedContinuation { continuation in
            provider.loadItem(forTypeIdentifier: typeIdentifier, options: nil) { item, _ in
                if let url = item as? URL {
                    continuation.resume(returning: readFile(at: url, defaultFilename: defaultFilename))
                } else if let data = item as? Data {
                    continuation.resume(returning: .file(data, filename: defaultFilename ?? "shared_file.bin"))
                } else {
                    continuation.resume(returning: nil)
                }
            }
        }
    }

    /// Loads the provider's original image representation as-is. Photos lists
    /// a JPEG compatibility rendition first (e.g. `public.jpeg`,
    /// `com.apple.live-photo`, `public.heic`), so prefer any non-JPEG image
    /// type and only fall back to JPEG. Returns `nil` if it can't be loaded.
    private static func loadOriginalImage(_ provider: NSItemProvider) async -> SharedItem? {
        let imageTypes = provider.registeredTypeIdentifiers
            .compactMap { UTType($0) }
            .filter { $0.conforms(to: .image) && $0 != .livePhoto }
        guard let type = imageTypes.first(where: { $0 != .jpeg }) ?? imageTypes.first
        else { return nil }
        let suggestedName = provider.suggestedName

        return await withCheckedContinuation { continuation in
            _ = provider.loadFileRepresentation(forTypeIdentifier: type.identifier) { url, _ in
                // temp file is deleted once this handler returns, so read it here.
                guard let url, let data = try? Data(contentsOf: url), !data.isEmpty else {
                    continuation.resume(returning: nil)
                    return
                }
                continuation.resume(returning: .file(data, filename: imageFilename(
                    suggestedName: suggestedName, type: type, fallback: url.lastPathComponent
                )))
            }
        }
    }

    /// `suggestedName` with the extension for `type`. Only a trailing image
    /// extension is replaced, so dotted names like "Screenshot at 10.30.15"
    /// keep their full stem.
    private static func imageFilename(suggestedName: String?, type: UTType, fallback: String) -> String {
        guard let name = suggestedName, !name.isEmpty,
              let ext = type.preferredFilenameExtension
        else { return fallback }
        let nsName = name as NSString
        let hasImageExt = UTType(filenameExtension: nsName.pathExtension)?.conforms(to: .image) == true
        let stem = hasImageExt ? nsName.deletingPathExtension : name
        return "\(stem).\(ext)"
    }

    /// Reads the contents of a (possibly security-scoped) file URL into a
    /// `.file` SharedItem, falling back to `defaultFilename` if the URL has
    /// no usable last path component.
    private static func readFile(at url: URL, defaultFilename: String? = nil) -> SharedItem {
        let isSecured = url.startAccessingSecurityScopedResource()
        defer { if isSecured { url.stopAccessingSecurityScopedResource() } }
        let filename = url.lastPathComponent.isEmpty ? (defaultFilename ?? "shared_file.bin") : url.lastPathComponent
        let data = (try? Data(contentsOf: url)) ?? Data()
        return .file(data, filename: filename)
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

/// A single resolved share item — either text or file-like binary content.
enum SharedItem {
    case text(String)
    case file(Data, filename: String)
}

enum IncomingPayload {
    /// Exactly one attachment was resolved.
    case single(SharedItem)
    /// More than one attachment was resolved (e.g. a Photos multi-select).
    case multiple([SharedItem])
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
    @State private var progress: (completed: Int, total: Int)?
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        NavigationStack {
            ZStack {
                LinearGradient(
                    colors: colorScheme == .dark
                        ? [
                            Color(hue: 0.6, saturation: 0.3, brightness: 0.12),
                            Color(hue: 0.62, saturation: 0.4, brightness: 0.08),
                        ]
                        : [
                            Color(hue: 0.6, saturation: 0.06, brightness: 0.98),
                            Color(hue: 0.62, saturation: 0.1, brightness: 0.93),
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
                            .accessibilityHidden(true)
                        Text("Tether")
                            .font(.headline)
                            .foregroundStyle(.primary)
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

            Divider().background(.primary.opacity(0.15))

            // Route buttons — only show relevant ones
            VStack(spacing: 12) {
                switch payload {
                case .single(let item):
                    singleItemActions(for: item)

                case .multiple(let items):
                    ActionButton(
                        icon: "arrow.up.doc.fill",
                        label: "Send \(items.count) Files",
                        subtitle: multiItemSummary(items),
                        color: Color(hue: 0.75, saturation: 0.7, brightness: 0.9)
                    ) {
                        performMultiple(items)
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

    @ViewBuilder
    private func singleItemActions(for item: SharedItem) -> some View {
        switch item {
        case .text(let text):
            if let url = Self.webURL(text) {
                ActionButton(
                    icon: "safari",
                    label: "Open on Desktop",
                    subtitle: url.host() ?? "Opens in your default browser",
                    color: .blue
                ) {
                    perform(.openUrl(url.absoluteString))
                }
            }
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
                color: Color(hue: 0.15, saturation: 0.8, brightness: colorScheme == .dark ? 0.9 : 0.7)
            ) {
                perform(.otp(text, source: "iPhone Share"))
            }

        case .file(let data, let filename):
            if UTType(filenameExtension: (filename as NSString).pathExtension)?.conforms(to: .image) == true {
                ActionButton(
                    icon: "clipboard",
                    label: "Copy to Clipboard",
                    subtitle: "Paste the image on your desktop",
                    color: .cyan
                ) {
                    // The daemon only takes PNG; this also converts JPEG/HEIC.
                    if let png = UIImage(data: data)?.pngData() {
                        perform(.clipboardImage(png))
                    } else {
                        resultMessage = "Couldn't read this image."
                        didFail = true
                    }
                }
            }
            ActionButton(
                icon: "arrow.up.doc.fill",
                label: "Send as File",
                subtitle: filename,
                color: Color(hue: 0.75, saturation: 0.7, brightness: 0.9)
            ) {
                perform(.file(data, filename: filename))
            }
        }
    }

    /// The text as an http(s) link with a host, or nil when it is anything else
    /// (prose, a sentence containing a link, mailto:, tel:).
    private static func webURL(_ text: String) -> URL? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.contains(where: \.isWhitespace),
              let url = URL(string: trimmed),
              let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
              url.host() != nil
        else { return nil }
        return url
    }

    // MARK: Content Preview

    @ViewBuilder
    private var contentPreview: some View {
        switch payload {
        case .single(let item):
            singleItemPreview(for: item)

        case .multiple(let items):
            VStack(spacing: 6) {
                Image(systemName: "square.stack.3d.up.fill")
                    .font(.largeTitle)
                    .foregroundStyle(.purple)
                    .accessibilityHidden(true)
                Text("\(items.count) items")
                    .font(.subheadline)
                    .foregroundStyle(.primary)
                Text(multiItemSummary(items))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal)
            }
            .padding()

        case .unsupported:
            EmptyView()
        }
    }

    @ViewBuilder
    private func singleItemPreview(for item: SharedItem) -> some View {
        switch item {
        case .text(let text):
            VStack(alignment: .leading, spacing: 4) {
                Label("Text", systemImage: "text.quote")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Text(text)
                    .lineLimit(3)
                    .font(.body)
                    .foregroundStyle(.primary)
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(.primary.opacity(0.06))
                    .clipShape(RoundedRectangle(cornerRadius: 10))
            }
            .padding(.horizontal)

        case .file(_, let filename):
            VStack(spacing: 6) {
                Image(systemName: "doc.fill")
                    .font(.largeTitle)
                    .foregroundStyle(.purple)
                    .accessibilityHidden(true)
                Text(filename)
                    .font(.subheadline)
                    .foregroundStyle(.primary)
            }
            .padding()
        }
    }

    /// Short "name, name, name +N more" summary used in the multi-item
    /// preview and action button subtitle.
    private func multiItemSummary(_ items: [SharedItem]) -> String {
        let names = items.prefix(3).map { item -> String in
            switch item {
            case .text: return "Text"
            case .file(_, let filename): return filename
            }
        }
        let remainder = items.count - names.count
        let suffix = remainder > 0 ? " +\(remainder) more" : ""
        return names.joined(separator: ", ") + suffix
    }

    // MARK: Sending / Result

    private var sendingView: some View {
        VStack(spacing: 16) {
            ProgressView()
                .tint(.cyan)
                .scaleEffect(1.4)
            Text(sendingStatusText)
                .font(.subheadline)
                .foregroundStyle(.secondary)
        }
    }

    private var sendingStatusText: String {
        if let progress {
            return "Sending \(progress.completed) of \(progress.total)…"
        }
        return "Connecting to Tether..."
    }

    private func resultView(message: String, failed: Bool) -> some View {
        VStack(spacing: 16) {
            Image(systemName: failed ? "xmark.circle.fill" : "checkmark.circle.fill")
                .font(.system(size: 48))
                .foregroundStyle(failed ? .red : .green)
                .modifier(BounceEffect(isActive: !reduceMotion))
                .accessibilityHidden(true)
            Text(message)
                .multilineTextAlignment(.center)
                .foregroundStyle(.primary)
                .padding(.horizontal)
            if failed {
                Button("Close") { onCancel() }
                    .buttonStyle(.borderedProminent)
                    .tint(.secondary)
            }
        }
        .onAppear {
            // The sheet can close before VoiceOver would reach the message.
            AccessibilityNotification.Announcement(message).post()
            if !failed {
                DispatchQueue.main.asyncAfter(deadline: .now() + (UIAccessibility.isVoiceOverRunning ? 3 : 1.4)) {
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
        case .openUrl: return "Opened on desktop ✓"
        case .otp: return "OTP stored in vault ✓"
        case .file(_, let name): return "\(name) sent ✓"
        case .clipboardImage: return "Copied to clipboard ✓"
        }
    }

    private func performMultiple(_ items: [SharedItem]) {
        isSending = true
        progress = (0, items.count)
        let files = Self.filesForTransfer(items)

        Task {
            let results = await ShareSender.sendFiles(files) { completed, total in
                Task { @MainActor in
                    progress = (completed, total)
                }
            }

            await MainActor.run {
                isSending = false
                progress = nil

                let succeeded = results.filter {
                    if case .success = $0 { return true }
                    return false
                }.count
                let total = results.count

                if succeeded == total {
                    resultMessage = "\(total) of \(total) sent ✓"
                    didFail = false
                } else {
                    resultMessage = "\(succeeded) of \(total) sent — \(total - succeeded) failed"
                    didFail = succeeded == 0
                }
            }
        }
    }

    /// Converts resolved items into (data, filename) pairs for `ShareSender.sendFiles`.
    /// Text items don't have a single obvious destination when batched with
    /// other files (unlike the single-item case's dedicated clipboard/OTP
    /// actions), so they're encoded as plain-text files instead.
    private static func filesForTransfer(_ items: [SharedItem]) -> [(data: Data, filename: String)] {
        var textCounter = 0
        return items.map { item in
            switch item {
            case .file(let data, let filename):
                return (data, filename)
            case .text(let text):
                textCounter += 1
                let filename = textCounter == 1 ? "shared_text.txt" : "shared_text_\(textCounter).txt"
                return (Data(text.utf8), filename)
            }
        }
    }
}

// MARK: - ActionButton

private struct ActionButton: View {
    let icon: String
    let label: LocalizedStringKey
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
                .accessibilityHidden(true)
                VStack(alignment: .leading, spacing: 2) {
                    Text(label)
                        .font(.body.weight(.semibold))
                        .foregroundStyle(.primary)
                    Text(subtitle)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer()
                Image(systemName: "chevron.right")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.tertiary)
                    .accessibilityHidden(true)
            }
            .padding(.horizontal, 14)
            .padding(.vertical, 12)
            .background(.primary.opacity(0.05))
            .clipShape(RoundedRectangle(cornerRadius: 14))
            .overlay(
                RoundedRectangle(cornerRadius: 14)
                    .stroke(.primary.opacity(0.08), lineWidth: 1)
            )
        }
        .buttonStyle(.plain)
    }
}

// Indefinite bounce needs iOS 18; the extension still supports iOS 17.
private struct BounceEffect: ViewModifier {
    let isActive: Bool

    func body(content: Content) -> some View {
        if #available(iOS 18.0, *) {
            content.symbolEffect(.bounce, isActive: isActive)
        } else {
            content
        }
    }
}

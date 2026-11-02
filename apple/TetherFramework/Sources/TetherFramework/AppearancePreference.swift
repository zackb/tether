//
//  AppearancePreference.swift
//  TetherFramework
//
//  The user's light/dark choice, stored in the App Group so the Share
//  Extension renders in the same appearance as the main app.
//

import SwiftUI

public enum AppearancePreference: String, CaseIterable, Identifiable, Sendable {
    case system
    case light
    case dark

    public static let storageKey = "TetherAppearance"

    // Shared UserDefaults suite backed by the App Group container.
    public static var sharedDefaults: UserDefaults {
        UserDefaults(suiteName: CertificateManager.appGroupID) ?? .standard
    }

    /// The stored preference, or `.system` when unset or unrecognized.
    public static var current: AppearancePreference {
        sharedDefaults.string(forKey: storageKey).flatMap(Self.init(rawValue:)) ?? .system
    }

    public var id: String { rawValue }

    /// The scheme to force, or nil to follow the system.
    public var colorScheme: ColorScheme? {
        switch self {
        case .system: nil
        case .light: .light
        case .dark: .dark
        }
    }
}

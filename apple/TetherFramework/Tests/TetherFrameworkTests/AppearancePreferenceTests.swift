//
//  AppearancePreferenceTests.swift
//  TetherFrameworkTests
//

import Foundation
import Testing
@testable import TetherFramework

struct AppearancePreferenceTests {
    @Test func mapsToColorScheme() {
        #expect(AppearancePreference.system.colorScheme == nil)
        #expect(AppearancePreference.light.colorScheme == .light)
        #expect(AppearancePreference.dark.colorScheme == .dark)
    }

    // A fresh install (or a value from a future version) must follow the system.
    @Test func defaultsToSystem() {
        let defaults = AppearancePreference.sharedDefaults
        let key = AppearancePreference.storageKey
        let saved = defaults.string(forKey: key)
        defer { defaults.set(saved, forKey: key) }

        defaults.removeObject(forKey: key)
        #expect(AppearancePreference.current == .system)

        defaults.set("sepia", forKey: key)
        #expect(AppearancePreference.current == .system)

        defaults.set(AppearancePreference.light.rawValue, forKey: key)
        #expect(AppearancePreference.current == .light)
    }
}

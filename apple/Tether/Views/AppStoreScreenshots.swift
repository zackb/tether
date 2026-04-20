//
//  AppStoreScreenshots.swift
//  Tether
//
//  Curated views configured specifically for taking App Store screenshots 
//  via the Xcode Simulator without needing to stub network connections.
//

import SwiftUI

#if DEBUG

// MARK: - Screenshot Configurations

// A container view designed perfectly to show off the App Store screenshots 
// for the main active states of the Tether application.
struct AppStoreScreenshots: View {
    @State private var selectedTab = 0
    
    var body: some View {
        TabView(selection: $selectedTab) {
            DashboardView()
                .tabItem { Label("Dashboard", systemImage: "network") }
                .tag(0)
            
            ClipboardView()
                .tabItem { Label("Clipboard", systemImage: "doc.on.clipboard") }
                .tag(1)
            
            FilesView()
                .tabItem { Label("Files", systemImage: "folder") }
                .tag(2)
            
            SettingsView()
                .tabItem { Label("Settings", systemImage: "gear") }
                .tag(3)
        }
        .environment(TetherViewModel.previewMock) // Inject perfect screenshot mock data
        .tint(.teal)
    }
}

// MARK: - Previews

// Use Xcode Previews to configure your simulator to exactly what you need.
// Run this specific preview on the Simulator and use the simulator's
// built-in "Save Screen" (Cmd+S) or `xcrun simctl io booted screenshot` feature.
#Preview("App Store: Full App (Dark)") {
    AppStoreScreenshots()
        .preferredColorScheme(.dark)
}

#Preview("App Store: Full App (Light)") {
    AppStoreScreenshots()
        .preferredColorScheme(.light)
}

#Preview("App Store: Pairing View") {
    // A mock for the pairing flow in motion
    let pairingMock = TetherViewModel()
    // It is normally an error to set immutable properties, but since we are in 
    // a preview, we can setup the view manually, or we can use the default state
    // modified slightly if needed.
    
    return PairingView()
        .environment(pairingMock)
        .preferredColorScheme(.dark)
}

#endif

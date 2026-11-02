//
//  TetherApp.swift
//  Tether
//
//  Created by Zack Bartel on 4/12/26.
//

import SwiftUI
import TetherFramework

@main
struct TetherApp: App {
    @State private var viewModel = TetherViewModel()
    @Environment(\.scenePhase) private var scenePhase
    @AppStorage(AppearancePreference.storageKey, store: AppearancePreference.sharedDefaults)
    private var appearance: AppearancePreference = .system

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(viewModel)
                .preferredColorScheme(appearance.colorScheme)
                .onAppear {
                    viewModel.initialize()
                }
                .onChange(of: scenePhase) { _, newPhase in
                    viewModel.handleScenePhase(newPhase)
                }
            /*
            AppStoreScreenshots()
                          .preferredColorScheme(.dark)
             */
        }
    }
}

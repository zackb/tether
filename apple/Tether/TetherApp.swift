//
//  TetherApp.swift
//  Tether
//
//  Created by Zack Bartel on 4/12/26.
//

import SwiftUI

@main
struct TetherApp: App {
    @State private var viewModel = TetherViewModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(viewModel)
                .preferredColorScheme(.dark)
                .onAppear {
                    viewModel.initialize()
                }
                .onChange(of: scenePhase) { _, newPhase in
                    viewModel.handleScenePhase(newPhase)
                }
        }
    }
}

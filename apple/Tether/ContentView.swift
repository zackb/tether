//
//  ContentView.swift
//  Tether
//
//  Created by Zack Bartel on 4/12/26.
//

import SwiftUI

struct ContentView: View {
    @Environment(TetherViewModel.self) private var viewModel

    var body: some View {
        @Bindable var vm = viewModel

        TabView {
            Tab("Dashboard", systemImage: "antenna.radiowaves.left.and.right") {
                DashboardView()
            }

            Tab("Clipboard", systemImage: "clipboard") {
                ClipboardView()
            }

            Tab("Files", systemImage: "arrow.up.arrow.down") {
                FilesView()
            }

            Tab("Settings", systemImage: "gearshape") {
                SettingsView()
            }
        }
        .tint(.teal)
        .sheet(isPresented: $vm.showPairingSheet) {
            PairingView()
                .environment(viewModel)
                .interactiveDismissDisabled()
        }
        .alert("Error", isPresented: .init(
            get: { viewModel.errorMessage != nil },
            set: { if !$0 { vm.errorMessage = nil } }
        )) {
            Button("OK") { vm.errorMessage = nil }
        } message: {
            Text(viewModel.errorMessage ?? "")
        }
    }
}

#Preview {
    ContentView()
        .environment(TetherViewModel())
}

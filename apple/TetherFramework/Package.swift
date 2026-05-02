// swift-tools-version: 5.9
//
//  Package.swift
//  TetherFramework
//
//  Shared library used by both the Tether main app and the TetherShareExtension.
//  Contains all protocol, networking, and certificate management code.
//

import PackageDescription

let package = Package(
    name: "TetherFramework",
    platforms: [
        .iOS(.v17),
        .macOS(.v14),
    ],
    products: [
        .library(
            name: "TetherFramework",
            targets: ["TetherFramework"]
        ),
    ],
    targets: [
        .target(
            name: "TetherFramework",
            path: "Sources/TetherFramework"
        ),
    ]
)

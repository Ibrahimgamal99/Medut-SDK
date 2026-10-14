// swift-tools-version:5.9
//
// MedutKit — Swift binding for the Medut SIP SDK (iOS).
//
// The native core ships as Medut.xcframework (header + module map), built on
// macOS by scripts/build-ios.sh or fetched from CI by scripts/fetch-prebuilt.sh
// into Frameworks/ next to this file, where it is committed.  The Flutter
// plugin vendors the same copy.

import PackageDescription

let package = Package(
    name: "MedutKit",
    platforms: [.iOS(.v13)],
    products: [
        .library(name: "MedutKit", targets: ["MedutKit"]),
    ],
    targets: [
        .binaryTarget(name: "Medut", path: "Frameworks/Medut.xcframework"),
        // The app-owned audio device: a VoiceProcessingIO render callback,
        // in Objective-C because it has to be C on the realtime thread anyway.
        .target(
            name: "MedutAudioEngine",
            dependencies: ["Medut"],
            path: "Sources/MedutAudioEngine",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("AudioToolbox"),
            ]
        ),
        .target(
            name: "MedutKit",
            dependencies: ["Medut", "MedutAudioEngine"],
            path: "Sources/MedutKit",
            linkerSettings: [
                .linkedFramework("AVFoundation"),
                .linkedFramework("Network"),
            ]
        ),
        .testTarget(
            name: "MedutKitTests",
            dependencies: ["MedutKit", "Medut"],
            path: "Tests/MedutKitTests"
        ),
    ]
)

// swift-tools-version:5.9
//
// MedutKit — Swift binding for the Medut SIP SDK (iOS).
//
// The native core ships as Medut.xcframework (header + module map), built on
// macOS by scripts/build-ios.sh or fetched from CI by scripts/fetch-prebuilt.sh
// into bindings/flutter/ios/Frameworks, where it is committed.  Sources/ and
// Frameworks/ here are symlinks into bindings/flutter/ios: the Flutter plugin's
// pod compiles the same files, and CocoaPods cannot see through a symlink,
// so the real directories live on its side.

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
                .linkedFramework("CallKit", .when(platforms: [.iOS])),
            ]
        ),
        .testTarget(
            name: "MedutKitTests",
            dependencies: ["MedutKit", "Medut"],
            path: "Tests/MedutKitTests"
        ),
    ]
)

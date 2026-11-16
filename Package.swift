// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Harmonizer",
    platforms: [.macOS(.v13)],
    targets: [
        // Realtime DSP + CoreAudio IOProc. Pure C++ so nothing Swift runs on the audio thread.
        .target(
            name: "HarmonizerDSP",
            path: "Sources/HarmonizerDSP",
            linkerSettings: [.linkedFramework("CoreAudio"), .linkedFramework("Accelerate")]
        ),
        // The app: SwiftUI controls over Metal-rendered surfaces.
        .executableTarget(
            name: "Harmonizer",
            dependencies: ["HarmonizerDSP"],
            path: "Sources/Harmonizer"
        ),
        // Offline sanity check / benchmark of every algorithm.
        .executableTarget(
            name: "hzbench",
            dependencies: ["HarmonizerDSP"],
            path: "Sources/hzbench"
        ),
    ],
    cxxLanguageStandard: .cxx17
)

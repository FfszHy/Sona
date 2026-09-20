// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Sona",
    platforms: [.macOS("26.0")],
    targets: [
        .executableTarget(
            name: "Sona",
            path: "Sources/Sona",
            linkerSettings: [
                .linkedFramework("CoreAudio"),
                .linkedFramework("AppKit"),
                .linkedFramework("ServiceManagement"),
            ]
        ),
    ]
)

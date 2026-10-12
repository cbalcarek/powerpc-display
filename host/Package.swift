// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "PowerPCDisplayHost",
    platforms: [.macOS(.v14)],
    targets: [
        // shared with the PowerPC client (g4/ compiles the same file)
        .target(name: "CCodec", path: "Sources/CCodec"),
        // Bonjour browse/resolve (dns_sd, part of libSystem)
        .target(name: "CDiscovery", path: "Sources/CDiscovery"),
        // thin Objective-C wrapper around macOS's virtual display API
        .target(
            name: "CVirtualDisplay",
            path: "Sources/CVirtualDisplay",
            linkerSettings: [.linkedFramework("CoreGraphics"), .linkedFramework("Foundation")]
        ),
        .executableTarget(
            name: "PowerPCDisplay",
            dependencies: ["CCodec", "CVirtualDisplay", "CDiscovery"],
            path: "Sources/G4DisplayHost",
            linkerSettings: [
                .linkedFramework("ScreenCaptureKit"),
                .linkedFramework("AppKit"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("ServiceManagement"),
                .linkedFramework("SwiftUI"),
            ]
        ),
    ]
)

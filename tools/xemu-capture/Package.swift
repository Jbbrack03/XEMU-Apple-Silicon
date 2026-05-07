// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "xemu-capture",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "xemu-capture", targets: ["xemu-capture"]),
    ],
    targets: [
        .executableTarget(
            name: "xemu-capture",
            path: "Sources/xemu-capture"),
    ]
)

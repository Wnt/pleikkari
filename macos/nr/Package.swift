// swift-tools-version: 6.2
//
// plknr: the neural-rendering engine of the native macOS client, behind a C interface.
//
// It runs the recovered DLSS 5 Neural Rendering network on Apple Silicon through
// MLX-DLSS (Apache-2.0, pinned by revision). The app loads libplknr.dylib at run time, so
// the CMake build of the client needs neither Swift nor MLX, and a client without the
// library simply has no neural rendering. No model is bundled or downloaded: the user
// supplies a .dlssmodel made from their own nvngx_dlssnr.dll (tools/prepare-model.sh).
import PackageDescription

let package = Package(
  name: "plknr",
  platforms: [.macOS(.v14)],
  products: [
    .library(name: "plknr", type: .dynamic, targets: ["PlkNR"]),
    .executable(name: "plk-nr-bench", targets: ["plk-nr-bench"]),
  ],
  dependencies: [
    .package(
      url: "https://github.com/iamwavecut/MLX-DLSS",
      revision: "e6e030caabe5f023bf367dd8328718f7ef8fcf3e"
    )
  ],
  targets: [
    // The C interface: one header, shared with the app (which includes it directly).
    .target(name: "CPlkNR"),
    .target(
      name: "PlkNR",
      dependencies: [
        "CPlkNR",
        .product(name: "DLSSCore", package: "MLX-DLSS"),
        .product(name: "DLSSMLX", package: "MLX-DLSS"),
      ],
      linkerSettings: [
        .linkedFramework("CoreVideo"), .linkedFramework("IOSurface"), .linkedFramework("Metal"),
      ]
    ),
    .executableTarget(
      name: "plk-nr-bench",
      dependencies: ["PlkNR", "CPlkNR"],
      linkerSettings: [
        .linkedFramework("CoreGraphics"), .linkedFramework("ImageIO"),
        .linkedFramework("UniformTypeIdentifiers"),
      ]
    ),
  ],
  swiftLanguageModes: [.v5]
)

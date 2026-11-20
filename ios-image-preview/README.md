# Installed iOS ImageIO preview

A new, independent preview component, not an existing MadMedia public SDK,
mobile stb port, or full engine qualification. It does not modify the original
CMake configuration or enable engine, transfer, curl, SQLite, platform leases,
observation, downloader or C ABI gates.

The versioned preview C function accepts one JPEG memory buffer, uses ImageIO
to inspect and decode an oriented thumbnail, draws it into fresh sRGB pixels
with CoreGraphics, and encodes a new JPEG with explicit quality. It never
upscales and supplies no input metadata to the encoder. Output may contain
system-generated JPEG/color metadata. This is not a blanket metadata forensic
guarantee. Pixel/input/edge/quality/output limits and caller buffer sizing are
documented in the header. No API accepts paths or uses the network.

[Image sources](https://developer.apple.com/documentation/imageio/cgimagesource),
[thumbnail creation](https://developer.apple.com/documentation/imageio/cgimagesourcecreatethumbnailatindex(_:_:_:)),
[image destinations](https://developer.apple.com/documentation/imageio/cgimagedestination).

The distinct branch `checkpoint/madmedia-ios-image-preview` is the only
candidate hosted trigger. The stock arm64 macOS runner requires Xcode 26.2
(17C52), SDK/Simulator 26.2. A fixed two-source allowlist is copied into a
disposable producer. Apple clang and libtool produce a real simulator archive;
platform and arm64 are checked. The archive and one public preview header are
installed to a prefix, moved to a separate UIKit host app, and the disposable
producer and build directories are deleted before consumer configuration.

The app owns the codec call from the installed archive, not a static mock or
XCTest-side reimplementation. Exact named tests exercise (1) synthetic JPEG
decode/downsize/encode/redecode, bounds/no enlargement, broad pixel fidelity,
lossy quality response, and the absence of the seeded TIFF Artist field in all
three outputs (high, low, no-upscale); (2) generic invalid bytes, logical bounds, quality and
capacity without output writes. No production images, uploads, files as API
inputs, third-party additions or device signing/testing. The simulator uses
local ad-hoc build signing only.

Local `python3 ios-image-preview/test_check.py` tests only the generated app,
mocked package staging, exact named result checks and workflow boundary. The
real archive, Apple API behaviour and native Simulator pass must be validated
by the exact hosted result before making any qualification claim.

The qualification queries the exact available iOS 26.2 runtime and creates a
private Simulator UUID. Boot and readiness failures have separate labels;
there are no retries. The app is compiled only as an ARM64 Simulator binary,
checked again by lipo after linking, and both real test entrypoints check the
running app reports iOS 26.2. This avoids depending on execution of an unrelated
guest utility before tests. The exact local cause of a prior combined boot-stage
failure was not observed.

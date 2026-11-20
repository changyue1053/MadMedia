# Compile-only iOS Simulator link matrix

Separate evidence for the App link failure, based on public
`7096eb0d2636ff58432c7bc2c12445c5a0f54eea`. No codec, original project/test
workflow, normal build defaults, engine, release or user-device changes.

The branch `checkpoint/madmedia-ios-image-link-ab` verifies the exact SHA and
uses a stock ARM64 macOS runner, Xcode 26.2/17C52, iOS Simulator SDK 26.2. It
runs the original simulator archive/header install and relocation once,
deletes the disposable producer, and copies the byte-identical archive into
three independent consumers. All link actual unchanged App.mm:

- Manual: Apple clang++ for arm64 iOS 15+ Simulator, absolute installed .a.
- PBX: existing generated project, existing archive reference in the App
  link phase, build only App on the generic Simulator destination.
- Explicit: same generated App project/configuration and source; only remove
  the file ref from the link phase and pass the quoted exact installed path
  to the linker. No extra library search directory or fallback.

The two project builds have distinct clean derived directories, no test
action, no code signing. No boot, XCTest, application execution, raw logs,
artifacts, downloads or installation on a user device. Every PASS requires a
real linked ARM64 iOS Simulator Mach-O with the defined codec function and
ImageIO/CoreGraphics dependencies. Native compile/link results are not a
substitute for the separate iOS runtime test or macOS host result.

A failure classified as this installed archive in the PBX case, with both
manual and explicit variants verified, confirms the archive resolution
hypothesis. If all three verify, it only means this App-only experiment did
not reproduce the failure. Other outcomes fail the diagnostic job instead
of guessing a root cause. Missing-lib diagnostics use allowlisted categories;
no unknown filenames or paths are printed. Local tests validate only generated
project and result structure, not Apple tooling behavior.

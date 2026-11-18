# M01a: independent native development baseline

Status: implemented and independently reviewed; approved for M01a source publication. Remote publication verification is recorded separately by the coordinator. This is a bounded substage of M01, not completion of all SDK work.

## Provenance

67 files from owner-provided IMMedia `immedia/` at commit `4751411da52e84a46d34d759de0f9f873a332ed8`. Original file identities in `docs/source-manifest.json`. No source history, root architecture HTML, credentials or prebuilt binaries copied. First-party license was absent and remains unassigned; no new reuse permission is invented.

## Delivered

- Native C++17 implementation, 52 original unit cases, 23 mock-service e2e scenarios and original fuzz/tools retained.
- `MadMedia::core` install/export package, Mad public façade, independently linked installed consumer.
- Default Noop/custom observer; optional MadObs contract-only adapter and `MadMedia::obs` optional installed component. No sibling runtime dependency.
- Finite telemetry dimensions only, adapter tests, callback exceptions contained.
- GCC portability includes; exception-safe partial initialization for Engine, ComputePool and HttpClient; synchronous callback-thread query rejected (Err::State) rather than deadlocking.
- Thread-create failure regression: first through seventh creation on this host; on small hosts cap injection to last actual creation, never skip.
- Original stb sanitizer exclusion removed. Found/repaired signed-left-shift UB in JPEG bit buffer by using unsigned buffer and matching internal parameters. All original license blocks retained.
- Third-party notices and Wolt blurhash MIT license added and installed; no first-party license chosen.

## Verified locally

Linux x86_64, GCC 14.2, CMake 4.4.5; system curl 8.14.1. Local SQLite dependency setup used a header from existing libsqlite3-sys-0.30.1 registry sources and system libsqlite3.so.0; these local build dependencies are not bundled. This is not a portable release toolchain validation.

- Native build including tools: PASS.
- CTest native: 11/11 PASS (52 unit cases + 23 e2e scenarios + observer + seven failure injections + callback query).
- Optional MadObs SDK tests: 10/10 PASS (using supplied MadObs observer contract only).
- Installed core-only consumer: PASS, prints `MadMedia 0.1.0-dev native link verified` and calls the out-of-line engine initializer.
- Installed `COMPONENTS Obs` consumer: PASS, verifies mapped event/finite enum filtering.
- ASan + UBSan: 10/10 unit/SDK CTest entries PASS with `UBSAN_OPTIONS=halt_on_error=1`, `ASAN_OPTIONS=detect_leaks=0` after JPEG fix.
- LeakSanitizer: BLOCKED, environment reports ptrace incompatibility. No leak-clean claim.
- ASan + UBSan e2e: 1/1 PASS (23 scenarios), with leak detection disabled for the same environment limitation.
- TSan, fuzz campaign, production control service, device runtime, iOS/Android/OHOS packages and remote CI: NOT VERIFIED.

## Commands

Standard developer commands are in README. Local dependency overrides for this environment were `-DSQLite3_INCLUDE_DIR=<local-header-dir> -DSQLite3_LIBRARY=<system-library-link>`. Optional observer build adds `-DMADMEDIA_WITH_MADOBS=ON -DMADOBS_INCLUDE_DIR=<MadObs include>`. Fault tests are Linux-only.

## Remaining / risks

Generic UploadProtocol/CredentialProvider injection and mobile background transfer are not complete. C++ façade is not stable C ABI. Telemetry observer is contractually nonblocking but shares callback thread; it cannot isolate arbitrary slow host code. Existing native diagnostic logs are not routed through the privacy-filtered adapter and can contain IDs/error text. Filesystem/task-id validation, process-wide curl lifecycle, journal failure semantics, parser fuzz and resource bounds remain pre-production audit work. Multi-SDK SSL/SQLite symbol qualification remains pending. No IMSDK or production Crypto/DB bridge implemented.

## Rollback

No original repository altered. Revert this development commit or choose a prior known-good MadMedia source snapshot; do not downgrade live journal formats blindly. Wire/journal names were preserved, not globally renamed. No production deployment performed.

## Independent-review correction: Release assertions

All SDK test `assert` calls were replaced with always-evaluated `CHECK`; side-effecting calls (upload/wait/result checks) now execute under NDEBUG. A separate `-O3 -DNDEBUG` probe verified the success path and deliberate failure exit (SIGABRT). Release + optional MadObs build passed all **12/12 CTest entries**, including original 52 unit + 23 e2e cases and fault/callback/adapter tests. CI now runs Debug/Release matrix. GCC14 Release's free-nonheap-object warning around JPEG vector reserve/push_back was avoided by equivalent SOI initialization before reserve, without weakening warning flags.

After that correction, full ASan+UBSan native CTest was rerun: **11/11 PASS** including e2e, with `detect_leaks=0` and `halt_on_error=1` (31.70s). Leak checking remains explicitly unverified.

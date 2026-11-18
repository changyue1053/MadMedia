# M02a verified result

Scope: Linux development C ABI v1, not a stable release or mobile package. No IMSDK, new runtime SDK dependency, or new third-party code. See C_ABI.md for API and failure semantics.

## Delivered

- C99 header and independently installed `MadMedia::ffi` static target, optional via MADMEDIA_BUILD_C_ABI.
- Opaque non-reused process handles; serialized operations and linearizable poll/destroy; no host callbacks.
- Bounded admission slots, progress coalescing, pinned first-terminal-wins snapshots, nonconsuming length/short-buffer probes, explicit saturation BUSY.
- Native initial journal-save failure now emits Failed instead of leaving an accepted task silent.
- OS-sourced 128-bit random task IDs with full-read/EINTR handling and fail-closed entropy failure before submission. No PRNG/counter fallback. Restart regression verifies two journal rows and distinct staging paths.
- Linux-only C ABI configuration gate. Other targets default OFF; explicit enable is rejected pending platform-specific provider validation.

## Validation

Executed on Linux x86_64, GCC 14.2, CMake 4.4.5:

- Release with optional MadObs adapter: 19/19 CTest cases passed.
- ASan + UBSan: 18/18 CTest cases passed (`ASAN_OPTIONS=detect_leaks=0`, `UBSAN_OPTIONS=halt_on_error=1`). LeakSanitizer is unavailable in this environment; no leak-clean claim.
- Installed package, separate pure-C consumer compiled/linked against `MadMedia::ffi` and ran successfully.
- C ABI OFF / tests OFF standalone core build passed.
- Pure C header, admission saturation, terminal retention, concurrent close, journal fault, 10,000 progress coalescing, callback-copy sticky failure, entropy open/read failure and short read, independent-process restart all covered.
- Independent review reproduced then closed the cross-process task-ID overwrite defect, verified entropy fail-closed and the Linux gate. Release tests use non-elidable CHECK.

## Limits

Not yet validated on Android, iOS or HarmonyOS. No platform packages, language wrappers, generic service ports, C recovery/pause/resume/streaming/observer APIs, process-wide memory cap or concurrent data-directory ownership support. This is a reviewed development stage, not a release. First-party distribution license choice remains pending; existing notices and third-party licenses are unchanged. No release tag is created.

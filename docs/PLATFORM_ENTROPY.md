# Platform entropy candidates (M02b, not qualified)

The C ABI now calls one private fixed-size entropy port. Linux remains the only enabled provider. It reads 16 bytes from the OS, retries EINTR and short reads, and fails closed without fallback. Output is zero on failure; no caller injection is exposed. This port is for operation IDs, not a public cryptographic primitive.

Candidate source files are deliberately NOT selected by production CMake. Non-Linux C ABI stays disabled by default and explicit enable is rejected. Host tests use test-only stand-in declarations and cannot prove target SDK ABI, linking, device entropy, or lifecycle. Mock headers are never installed.

| Candidate | Official API / link requirement | Implementation status |
|---|---|---|
| Apple / iOS | SecRandomCopyBytes, Security.framework, check returned status | Candidate + host success/error tests; deployment availability must be checked with selected Xcode SDK |
| Android | bionic getrandom, libc, available API 28+ | Candidate + host EINTR/short-read/error/EOF tests; no NDK/device build |
| HarmonyOS | OH_CryptoRand_Create/GenerateRandom, CryptoArchitectureKit headers, libohcrypto.so, API 20+ | Candidate + host context/blob cleanup and error/length tests; no SDK/device build |

The API floors describe these candidates, not an approved SDK minimum OS support policy. Lower Android/HarmonyOS versions require a separately reviewed provider, not a silent weak fallback. Mobile packaging also needs target libcurl and SQLite builds, ABI symbols, architecture slices and dependency conflict qualification.

## Evidence

- [Apple SecRandomCopyBytes](https://developer.apple.com/documentation/security/secrandomcopybytes(_:_:_:)): status must be checked before consuming output; Security framework API.
- [Android bionic sys/random.h](https://android.googlesource.com/platform/bionic/+/master/libc/include/sys/random.h): getrandom declaration is introduced in API 28.
- [HarmonyOS crypto_rand.h](https://developer.huawei.com/consumer/en/doc/harmonyos-references/capi-crypto-rand-h/): API 20, library libohcrypto.so, context destruction and output-data release requirements.
- [HarmonyOS native crypto examples](https://developer.huawei.com/consumer/en/doc/HarmonyOS-Guides/crypto-aes-sym-encrypt-decrypt-ndk/): crypto_common.h and Crypto_DataBlob fields.

## Required gate to enable each target

1. Choose supported minimum OS/API and obtain matching official toolchain.
2. Compile provider against real headers, link required framework/library and independently inspect imports/exports.
3. Build and run complete native+C ABI regression on simulator/device as applicable, including entropy failure injection and process restart persistence.
4. Verify account-private paths, background/foreground lifecycle, poll/destroy races and cancellation on target.
5. Test standalone installed/package consumer and combination with sibling SDKs; retain notices.
6. Independent review, then change the target CMake gate. Host-model success alone never authorizes this step.

## CI fortification regression

M02a CI's Release-only short-read fault-injection failure was reproduced with `_FORTIFY_SOURCE=3`: glibc rewrites read to `__read_chk`, bypassing the old wrapper after the fake open substituted /dev/null. This was an injection gap, not evidence of production entropy failure. The test now intercepts both entry points and delegates invalid lengths to the real checked function. CI explicitly retains level-3 fortification in Release. Local fortified reproduction fails before and passes after this change.

## Test storage isolation

A local full run encountered staging/fixture failures while the shared /tmp tmpfs was full. The original failing operation did not record errno, so disk exhaustion is a correlated environment observation, not a proven error-code diagnosis. Logs are preserved and tests rerun with adequate storage. The test fixture helper now rejects mkdtemp failure (never falls back to /tmp), accepts an absolute TMPDIR, and cleans only directories it created in the current process on normal exit. It never sweeps old fixtures or user directories; abnormal termination can still leave artifacts for inspection.

## Final local results

With isolated TMPDIR on a volume with sufficient capacity: Release + MadObs 23/23, ASan+UBSan 22/22 (leak detection disabled), explicit FORTIFY3 Release 22/22, installed pure-C consumer passed. These counts include 3 host-model platform tests, not 3 target-platform tests. The earlier resource-limited batch was stopped and is not counted as a pass. C ABI entropy tests additionally assert injected read counts and exercise an actual EIO branch as well as EOF.

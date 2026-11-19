# Android capability-only cross-link preview

This optional, local checker must be given an **already installed** NDK r28c
(28.2.13676358) on Linux x86-64. It does not provision tools or run target code.

```sh
python3 android-capability-preview/test_check.py
python3 android-capability-preview/check.py --ndk "$ANDROID_HOME/ndk/28.2.13676358"
```

- API 24: cross-link the independent POSIX advisory lease probe retained in
  the test-only Apple capability folder. No engine or entropy dependency.
- API 28: cross-link the unchanged shipped candidate
  `ffi/platform/entropy_android.cpp` with its canonical declaration from
  `core/platform/entropy.h`. Require an AArch64 Android PIE that imports
  bionic `getrandom`.
- API 24: compile/link the same entropy harness and provider. Accept only an
  explicit `getrandom` availability/declaration/symbol failure, not a wrong
  toolchain, missing header, unrelated source error or substitute provider.
  No provider fallback is included. Changed API support would need a
  separately reviewed OS provider.

This isolated gate never changes or enables the core/FFI CMake targets.
It stages five clean local files in a temporary directory and removes the
resulting ELF files. No APK, JNI, engine, curl/SQLite, package distribution,
runtime, emulator/device, randomness assurance or 16 KiB claim.

[Android NDK target flags](https://developer.android.com/ndk/guides/other_build_systems)
and [bionic getrandom declaration](https://android.googlesource.com/platform/bionic/+/master/libc/include/sys/random.h).

The separate preview workflow may install the exact official NDK on a hosted
runner with existing accepted agreement files only. The installer gets closed
stdin and only one exact package argument. The workflow rejects unreadable
or changed license directory contents, installation failure, and an unexpected
package receipt. No licenses, raw installation logs, or binaries are uploaded.

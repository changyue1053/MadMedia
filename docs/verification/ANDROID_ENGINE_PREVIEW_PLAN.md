# Android API 28 engine preview: decision and acceptance gates

**Decision: blocked.** This record does not enable the Android engine or C ABI.
The capability-only [CI run 44521956720](https://github.com/changyue1053/MadMedia/actions/runs/44521956720)
at `bffbc3b7f021eab8998c0758bf71d05598dfd11a` (tree
`2adfd1a2dd50b47cce018d81cee9f15ddc8c071e`) used NDK r28c to cross-link
the independent ARM64 lease at API 24 and the unchanged Android entropy provider
at API 28. The latter imports bionic `getrandom`; API 24 rejected that provider
explicitly. No Android code was executed, and those ELFs were not engine builds.

## Why the checked-in engine is not ready

- [CMake](../../CMakeLists.txt) requires Threads, CURL and SQLite3. It qualifies
  core ownership and entropy only on desktop Linux and explicitly rejects the
  C ABI on non-Linux targets. The dependencies must be Android-target builds.
- [Core entropy](../../core/platform/entropy.cpp) and [directory ownership](../../core/platform/directory_lock.cpp)
  deliberately fail closed on Android. The [bionic provider](../../ffi/platform/entropy_android.cpp)
  is not selected by the engine build, and the lease probe is test-only. Adding
  the provider to the core glob without removing the fallback would create
  two definitions of the same function.
- [Engine startup](../../core/engine/engine.cpp) opens its directory lease,
  staging area and [SQLite WAL journal](../../core/journal/journal.cpp) before
  creating transport/scheduler state. Its [data-plane adapter](../../core/protocol/transfer_port.h)
  can rewrite URL and headers within policy; it is not a transport executor.
  The [v1 C ABI](../../include/madmedia/madmedia_c.h) has no stream or provider ABI.
- [HTTP transport](../../core/transport/http.cpp) keeps redirects off, but that
  alone is not an Android TLS or CA policy. Existing [control](../../core/protocol/service_port.cpp)
  and [data](../../core/protocol/transfer_port.cpp) URL checks can accept HTTP.

## Ordered exit criteria

1. **Offline dependency identity and closure.** Require already-supplied,
   reviewed target inputs: SQLite amalgamation with WAL/thread support, libcurl
   and the exact chosen TLS backend, CA policy and transitive dependencies.
   Record independent official origin/version, expected digest, signatures
   where available, build flags and notices. A supplied checksum checks byte
   identity, not source authorship, provenance or redistribution rights.
   Build API 28 ARM64 only from verified inputs; reject host/unapproved Android
   libraries, implicit network fetches, and incomplete linkage. An independent
   consumer must link from installed target artifacts; inspect AArch64 ELF and
   every actual dynamic dependency. No tool download or signing in this gate.
2. **Experimental C++ engine, still opt-in.** Select exactly one entropy
   definition, the already checked-in API 28 provider; preserve zero output on
   failure and no fallback. Integrate and review the directory lease around
   app-private, anchored ownership and engine lifetime. Leave default targets,
   Linux behavior and the production C ABI unchanged. Prove refusal for API
   below 28 and insecure/unowned paths. In an authorized runtime later, test
   contention, release/reacquire and failure isolation before qualification.
3. **Localhost-only runtime, separately reported.** Start, stop and restart an
   independent consumer with a fresh internal app directory. Validate WAL mode
   and journal reopening, ownership, failed input producing no task row or
   staging file, and no request for rejected input. Separately submit synthetic
   file/stream bytes to a disposable loopback HTTPS fixture. Check byte digest,
   expected terminal result and the precise journal/receipt boundary; the
   receipt alone is not confirmation of media completion. Verify peer and
   hostname checks and rejection of wrong certificates, untrusted origins,
   plaintext credential-bearing destinations, and unsupported URL schemes.
   Use only synthetic CA/test material scoped to that fixture. Do not disable
   verification, alter any default crypto/TLS material, infer native libcurl
   trust from Android framework settings, or send live data or credentials.
   No physical device, production transfer or release claim follows from a
   build-only check.

The [repository notices](../../THIRD_PARTY_NOTICES.md) document SQLite as an
external public-domain dependency and libcurl as an external curl-licensed
dependency. Verify the exact TLS backend and other packaged components
separately; these notices do not grant a first-party MadMedia license or
authorize SDK distribution.

Primary references: [NDK native APIs](https://developer.android.com/ndk/guides/stable_apis),
[SQLite compilation](https://www.sqlite.org/howtocompile.html),
[SQLite copyright](https://www.sqlite.org/copyright.html),
[curl license](https://curl.se/docs/copyright.html),
[curl TLS verification and trust stores](https://curl.se/docs/sslcerts.html),
and [Android network security configuration](https://developer.android.com/privacy-and-security/security-config).

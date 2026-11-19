# Nonshipping Android combined dependency closure

This isolated build/link-only gate takes the [same official curl/Mbed TLS](../curl_tls/README.md)
and [SQLite](../sqlite/README.md) archives and a separately trusted preinstalled
NDK r28c (28.2.13676358), CMake and Ninja. The existing verifiers are unchanged.
No provisioning, download, signing, system install, runtime or distribution is
performed. Build subprocesses use the existing fail-closed disconnected runner.

```sh
python3 android-capability-preview/combined_deps/test_verify.py
python3 android-capability-preview/combined_deps/verify.py \
  --curl /trusted/curl.tar.xz --mbedtls /trusted/mbedtls.tar.bz2 \
  --sqlite /trusted/sqlite.zip --ndk /trusted/ndk/28.2.13676358 \
  --cmake /trusted/bin/cmake --ninja /trusted/bin/ninja
```

Keep only expected static archives and API header trees, hash them, relocate
the prefix to a different path containing a space, and delete all producer
directories. An independent C target links from explicit relative staged paths.
Require link-map provenance and defined symbols for both dependency families,
a narrow Android NDK dynamic dependency closure, no RPATH/RUNPATH, and every
final ELF `PT_LOAD` with power-of-two alignment >=16 KiB and congruent offsets
and virtual addresses. No outbound calls or default CA changes.

The result qualifies this single consumer link only. It does not prove every
archive member is used, multiple providers can coexist, Android/16 KiB device
runtime or APK packaging works, or MadMedia engine/ABI is ready.

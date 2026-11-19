# Offline Android curl and Mbed TLS compile/link probe

Nonshipping target-dependency qualification only. This does not download or
configure MadMedia's engine, ABI, curl transport, system trust or crypto defaults.
Nothing is installed to the system or redistributed. Nothing connects to an
endpoint, performs a transfer, or executes an Android binary.

## Publisher inputs

- [curl 8.23.0](https://curl.se/download.html), canonical
  `curl-8.23.0.tar.xz` (2,990,916 bytes), SHA-256
  `8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5` from the
  [official release asset metadata](https://api.github.com/repos/curl/curl/releases/tags/curl-8_23_0).
  The [publisher signing policy](https://curl.se/docs/verify.html) publishes a
  detached archive signature; this receipt verifies publisher digest, not that
  signature. Carry **COPYING** during any authorized distribution review.
- [Mbed TLS 4.1.2 LTS](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-4.1.2),
  canonical `mbedtls-4.1.2.tar.bz2` (6,552,353 bytes), SHA-256
  `392215e5e4846df04e2f4c4516079303bd73edde837d8f3e68fb4d4d7ddf0126`.
  The release page checksum agrees with official release asset metadata; no
  detached release signature is provided and the Git tag is unsigned. Use the
  canonical archive only, not GitHub auto-generated snapshots. It contains
  framework and TF-PSA-Crypto submodules and generated sources.
  Retain top-level and submodule LICENSE files. Mbed TLS is dual
  Apache-2.0 OR GPL-2.0-or-later; assess notices for the chosen license and exact
  components separately. These upstream notices do not license MadMedia.

## Gate

Supply separately verified official archives and already installed, trusted
Linux x86-64 NDK r28c (28.2.13676358), CMake and Ninja. The NDK receipt is a
version check, not attestation of arbitrary caller-supplied binaries.

```sh
python3 android-capability-preview/curl_tls/test_verify.py
python3 android-capability-preview/curl_tls/verify.py \
  --curl /trusted/curl-8.23.0.tar.xz --mbedtls /trusted/mbedtls-4.1.2.tar.bz2 \
  --ndk /trusted/ndk/28.2.13676358 --cmake /trusted/bin/cmake --ninja /trusted/bin/ninja
```

Verify exact size/hash before bounded unpacking regular files into a private
disposable directory. No links, traversal or archive metadata restores. Each
tool command uses a fresh Linux user+network namespace with a reduced
environment. If namespaces are unsupported the build fails closed. No tool or
source provisioning and no package discovery outside the explicit target
dependency/toolchain inputs.

Build static target-only mbedtls/x509/tfpsacrypto and libcurl with NDK
arm64-v8a/API 28. Native/host CA stores, default CA file/path, proxies, netrc
and alternate TLS backends must be absent from generated configuration.
Require actual HTTP(S)-only protocol summary. Delete producer directories;
link a distinct C consumer against explicit staged archives. Require target
ELF/interpreter, narrow NDK-only dynamic imports, no rpath, and actual
curl and Mbed TLS symbols. The executable has no perform/send call and is never
run by the checker. This does not qualify a 16 KiB device or any Android runtime.

**Separate future runtime gate:** use an isolated loopback TLS fixture with a
synthetic per-handle CA and checked hostname and peer verification. The
[pinned Mbed TLS backend](https://github.com/curl/curl/blob/curl-8_23_0/lib/vtls/mbedtls.c)
can use `CURLOPT_CAINFO_BLOB` but still considers an explicit CA path. Test
valid, wrong-CA and wrong-hostname cases with no default trust, proxies, outside
egress or live credentials. No TLS handshake or trust-enforcement claim
follows from this probe.

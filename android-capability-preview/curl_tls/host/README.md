# Linux host loopback TLS qualification (nonshipping)

This is a separate HOST acceptance target. It does not change the Android
dependency link gate, MadMedia engine/ABI, public transport, default trust,
crypto settings, licenses or workflows. It never installs tools or stages
real credentials. It makes no Android, device, transfer or SDK claim.

Prerequisites: Linux x86-64/glibc, non-root caller, trusted preinstalled
stock C compiler, binutils, OpenSSL CLI, Python 3 with SSL, iproute2, CMake and
Ninja. Supply the already acquired, exact [publisher archives](../README.md)
and pass trusted local CMake/Ninja paths. User/network namespaces must be
allowed by the host as-is; no sudo, altered permissions, AppArmor changes,
sysctls or fallback. The runner must be able to enable **only its new
namespace's** already-isolated loopback interface.

```sh
python3 android-capability-preview/curl_tls/host/test_host.py
python3 android-capability-preview/curl_tls/host/run_host.py \
  --curl /trusted/curl-8.23.0.tar.xz --mbedtls /trusted/mbedtls-4.1.2.tar.bz2 \
  --cmake /trusted/bin/cmake --ninja /trusted/bin/ninja
```

The existing source verifier checks exact size/SHA and version before
bounded safe staging. All build commands run disconnected in new user and
network namespaces. Explicit static dependency selection keeps only the
pinned libcurl/Mbed TLS stack; check generated HTTP(S)-only configuration,
absence of default CA/trust, proxies and netrc, and an independent x86-64
ELF with local curl/mbedtls symbols and no alternate dynamic TLS/curl.
No source or executable artifact is retained or uploaded.

The test supervisor verifies distinct user and network namespaces and a
single mapping from the original unprivileged UID. It inspects links and
IPv4/IPv6 routes before and after bringing up child loopback, refuses other
interfaces or routes, then launches all test activity within that one
namespace. Local TLS server binds only to an ephemeral `127.0.0.1` port.
OpenSSL CLI generates two disposable, unrelated test CAs and a short-lived
server leaf for `media.test`. Python SSL is server-only, not a client backend.
The C client checks exact linked runtime curl and Mbed TLS versions, checks
every setopt/CA blob setter, maps names explicitly to loopback without DNS,
uses HTTPS/peer/name verification, and makes fresh unshared connections.

Acceptance requires four ordered connections and **exactly** two complete
HTTP requests: correct CA/name → 200 and exact bounded bytes; wrong CA →
certificate-verification failure with zero HTTP response bytes; wrong
hostname using correct CA → same specific failure; correct CA/name again →
200 and exact bytes. The two negative cases must reach the same listener but
not deliver any HTTP request. Connection failures, timeouts, unsupported
options, malformed output or unexpected data never count as rejection proof.
Temporary certs, build sources and local binaries are removed. The portable
tests validate only synthetic policy/parser fixtures; run the full check to
claim Linux execution proof.

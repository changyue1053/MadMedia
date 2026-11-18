# Data-plane request adaptation (development C++ API)

`Config::data_plane` optionally supplies a shared immutable `DataPlanePort`. It receives only operation (UploadPart/DownloadPart), current signed URL and byte offset/length. It returns a URL and additional headers. It never receives local paths or file contents and cannot change method, storage, native range, integrity headers or curl execution. Empty keeps native behavior. C ABI v1 does not expose it.

## Credential boundary

A nonempty `data_plane_allowed_origins` is mandatory when enabling the port. Each entry is an explicit HTTP(S) origin, with no userinfo, path beyond '/', query or fragment. Before calling host code, the SDK parses the original signed URL using libcurl's URL API and requires its normalized scheme/host/port to match the allowlist. Thus a backend-supplied unknown origin does not even invoke a credential adapter. The returned URL must remain on that same origin. Scheme changes and port changes are cross-origin. Default ports and host case normalize. IPv6 zone identifiers are rejected rather than collapsed across interfaces. Any percent-encoded authority (including empty or raw zone syntax) is rejected before parser-version-specific behavior; percent-encoded hostnames are intentionally unsupported. Redirect following stays disabled in the HTTP transport, including same-origin redirects.

Use HTTPS in production; plain HTTP requires an explicit HTTP origin and is used only by the localhost test suite here. The host is responsible for choosing trusted origins and account-specific credential snapshots. URL userinfo, fragments, control characters and whitespace are rejected. URLs are capped at 8192 bytes. Extra headers are capped at 64/32KiB, reject control characters and duplicate names, and cannot override Host, Content-Length, Transfer-Encoding, Range, Content-Range, CRC, Expect, Content-Encoding connection framing or proxy-authorization fields. Existing native signed URLs may contain secret query parameters; neither the adapter nor application should log them.

## Lifecycle / failure

The port is retained with Engine shared ownership until destruction. Calls are synchronous, pure, prompt-return transforms on that engine's scheduler. A shared port must safely handle concurrent calls from different engines. Do not do I/O, wait, reenter Engine or destroy it from a transform. No protection against deliberately blocking host code is promised. Native producers must stop before destruction; concurrent native API/destruction is not added by this feature.

Credentials may be refreshed outside the scheduler. The tested example publishes an atomic versioned snapshot between an upload and a download; each request reads the current snapshot. No asynchronous refresh handshake, credential persistence, automatic refresh on 401, secure-store integration or auth-provider SDK is supplied.

Adapter exceptions, invalid results and disallowed origins become Protocol/Failed before transmission. When a data port is configured, task failure diagnostics are fixed `data operation failed` (with original Err code retained) before terminal/log/journal sinks, including native transport diagnostics that might mention URL data. This intentionally trades raw error text for credential safety. Task/media identifiers and normal successful events remain application data; do not put credentials in those fields. Existing unrelated logs are not claimed to have received a complete security audit.

## Verification scope and remaining blockers

Tests require a fixture Authorization header on the real mock data plane, upload an 800KiB file in multiple parts, externally rotate the credential snapshot, then range-download and compare every byte. They verify part counts, shared ownership through close, rejected cross-origin/invalid/framing inputs, no transfer on adapter failure and no following a cross-origin redirect to a second server. This exercises the native media-v1 provider, not arbitrary cloud storage interoperability or a replacement transfer engine.

Native C++ default task-ID generation still uses the legacy single-seed PRNG and is NOT equivalent to the C ABI's OS-128-bit ID provider. Treat that as an outstanding hardening blocker for broad production use. Three mobile platform packages/entropy/lifecycle remain unqualified. No release is made by this stage.

URL parsing follows the [libcurl URL API](https://curl.se/libcurl/c/curl_url_get.html): default ports are normalized, and zone IDs are separate from hostnames, so this adapter rejects zone IDs explicitly. Empty query/fragment syntax is also rejected in origin allowlist entries.

Final Linux verification: Release with MadObs 25/25, ASan+UBSan 24/24 (`detect_leaks=0`, not leak-clean), explicit FORTIFY3 Release 24/24; installed pure-C and C++ adapter consumers passed. Independent review reproduced the IPv6-zone origin issue; the uniform rejection plus raw-authority guard closed it, and the original probe now rejects both allowlist and mapping. No mobile build/device qualification is implied.

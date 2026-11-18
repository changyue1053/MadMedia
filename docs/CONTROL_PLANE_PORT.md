# Optional control-plane protocol port (source-level development API)

`Config::control_plane` accepts a shared immutable `ControlPlanePort`. Empty means the existing media-v1 protocol unchanged; no other Mad SDK is required. The C++ façade exports the port, operation and DTO aliases. This is not a stable binary ABI and is not exposed by C ABI v1 yet.

The engine builds its canonical media-v1 request, then `encode` converts method, absolute URL, headers and body to the host service's wire protocol. On a transport-success control response, `decode` converts HTTP status/body back to the existing canonical media-v1 semantics. Core validation/parsing still runs. Operations are Init, PartUrls, Complete, Challenge, DownloadUrl and MediaStatus. This is a wire-protocol translation port, not a replacement scheduler or storage protocol model.

Two independent localhost tests use JSON POST `/json/objects` with HTTP 201/object response, and text POST `/text/store` with HTTP 202/`STORED` response, both translated to canonical instant-upload completion. Existing native media-v1 e2e coverage remains. These two focused tests do not establish complete multipart interoperability with any production provider.

## Lifecycle and errors

- Shared ownership lasts through engine destruction; caller may release its own pointer after configuration. No runtime adapter replacement.
- Methods run serially on that engine's scheduler, not HTTP/callback thread. If a port is shared by engines it must be safe for concurrent calls across engines.
- Pure, bounded, prompt-return transformations only. No I/O, waits, Engine calls, synchronous queries or destruction from these methods. A blocking adapter can stall the scheduler; this version does not promise isolation from hostile code.
- Exceptions or error results become `Err::Protocol`/Failed with fixed `control protocol rejected`; adapter diagnostic strings are not forwarded into events/logs. Native transport errors bypass decoding and retain existing retry behavior. Native schema validation follows decode. All Protocol failures with a configured port are centrally redacted before events, logs and journal storage, including errors from the downstream native parsers.
- Retry invokes encode again. Callers must not perform non-idempotent side effects in transforms. Asynchronous token refresh is not implemented.

## Bounds and credentials

Encoded method is GET or POST. GET bodies are rejected. Absolute HTTP(S) URL is <=8192 bytes, has no control characters, fragment or userinfo. Headers are <=64 and <=32KiB total, have simple ASCII names with no control characters; Host, Content-Length and Transfer-Encoding cannot override transport framing. Encoded and decoded bodies are <=1MiB. Existing transport caps buffered input at 16MiB before decoding. This is not a total process memory cap, nor a cap on memory allocated inside a host-supplied adapter.

Authorization may be supplied in headers, never URL userinfo. Host must choose trusted destinations and enforce account isolation; it controls where its own credentials go. This SDK does not configure credentials or publish them. No adapter request/response body or header is added to observability. Entire pre-existing native logging has not been certified credential-free; do not put secrets into media identifiers or native error text.

Data-plane signed URLs, curl transfers, retry semantics, canonical response validation, persistence and media transforms remain native. Replacing transfer engines, arbitrary non-HTTP transports, async authentication refresh, production backend qualification and C-language adapter callbacks are NOT delivered here.

The native C++ Engine lifecycle rules still apply: producers stop before destruction; adapter injection does not make concurrent native Engine API/destruction safe. The separate C polling API has its own stronger registry-close serialization, but cannot configure this port yet.

## Verification record

Independent review found and reproduced a decoded unknown-result marker leaking through the native parser. Central Protocol-failure redaction was added before all event/log/journal sinks when a port is configured. The original independent reproducer passes after the fix. Seven engine-driven error regressions cover Init, PartUrls, Complete 200/409, Challenge, DownloadUrl and MediaStatus; terminal and journal assertions verify the fixed diagnostic. Request validation tests cover invalid URL/scheme/userinfo/header/framing/method/body and oversized decode input. These are control-plane tests, not a production-provider or full alternate multipart-transfer qualification.

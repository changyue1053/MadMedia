# Public API (0.1 development)

`MadMedia::core` exports `<madmedia/madmedia.h>` and source aliases for Config, Engine, requests, progress and errors. `madmedia::start` wraps user callbacks and optional observer, containing C++ exceptions. Direct `Engine::start` is upstream behavior without that wrapper.

No binary ABI compatibility is promised yet. Keep compiler/runtime consistent. Internal headers are installed under madmedia/internal solely to satisfy the façade; they are not supported extension points. Do not replace serialized state names, task IDs or protocol JSON keys with bulk rename.

Thread contract: control API enqueues commands; query synchronously waits outside callbacks; on the callback thread it immediately returns Err::State to prevent queue deadlock. User and observer callbacks run on the engine callback thread, not scheduler. They must return promptly, may not destroy engine, may not block waiting for callbacks. Close/destruction is from host lifecycle thread after host producers stop. Callback contexts remain alive through destruction. Concurrent destruction with API calls or observer replacement/unregistration is unsupported.

Observation v1 contains only TaskKind [0,1], TaskState [0,7], Err [0,13]. Optional MadObs adapter uses caller-assigned nonzero SDK ID, sdk_version=1, event_id=1, attributes 1=kind,2=state,3=error. Invalid enum input is filtered. No IDs/text/payloads are exposed. `invoked` counts callback invocation, `rejected` includes Noop/invalid/callback error, not successful export. Runtime schema admission must be configured separately.

The callback observer is contractually nonblocking, not isolated against a malicious or stalled observer. A dedicated bounded telemetry delivery queue is future work before claiming strong noninterference.

Initialization failures are reported as Err::Busy; created worker threads and CURLM are cleaned up. Destruction checks scheduler joinability. Callback exceptions are contained in the native callback worker too.

Optional source-level `Config::control_plane` translates control HTTP wire protocols. See [control-plane port](docs/CONTROL_PLANE_PORT.md) for synchronous pure-transform, lifecycle, bounds and redaction rules. No C ABI adapter injection, data-plane replacement or asynchronous authentication refresh is included.

# Public API (0.1 development)

`MadMedia::core` exports `<madmedia/madmedia.h>` and source aliases for Config, Engine, requests, progress and errors. `madmedia::start` wraps user callbacks and optional observer, containing C++ exceptions. Direct `Engine::start` is upstream behavior without that wrapper.

No binary ABI compatibility is promised yet. Keep compiler/runtime consistent. Internal headers are installed under madmedia/internal solely to satisfy the façade; they are not supported extension points. Do not replace serialized state names, task IDs or protocol JSON keys with bulk rename.

Thread contract: control API enqueues commands; query synchronously waits outside callbacks; on the callback thread it immediately returns Err::State to prevent queue deadlock. User and observer callbacks run on the engine callback thread, not scheduler. They must return promptly, may not destroy engine, may not block waiting for callbacks. Close/destruction is from host lifecycle thread after host producers stop. Callback contexts remain alive through destruction. Concurrent destruction with API calls or observer replacement/unregistration is unsupported.

Observation v1 contains only TaskKind [0,1], TaskState [0,7], Err [0,13]. Optional MadObs adapter uses caller-assigned nonzero SDK ID, sdk_version=1, event_id=1, attributes 1=kind,2=state,3=error. Invalid enum input is filtered. No IDs/text/payloads are exposed. `invoked` counts callback invocation, `rejected` includes Noop/invalid/callback error, not successful export. Runtime schema admission must be configured separately.

The callback observer is contractually nonblocking, not isolated against a malicious or stalled observer. A dedicated bounded telemetry delivery queue is future work before claiming strong noninterference.

Initialization failures are reported as Err::Busy; created worker threads and CURLM are cleaned up. Destruction checks scheduler joinability. Callback exceptions are contained in the native callback worker too.

Optional source-level `Config::control_plane` translates control HTTP wire protocols. See [control-plane port](docs/CONTROL_PLANE_PORT.md) for synchronous pure-transform, lifecycle, bounds and redaction rules. No C ABI adapter injection, data-plane replacement or asynchronous authentication refresh is included.

Optional `Config::data_plane` adapts data-request URL/extra headers inside an explicit origin allowlist. See [data-plane request port](docs/DATA_PLANE_PORT.md) for same-origin, redirect, credential, native transfer preservation and lifecycle constraints. It does not replace the transport or add async authentication.

Native task-ID defaults now share the Linux OS entropy provider with C ABI; new explicit IDs use restricted ASCII and duplicate admissions are rejected. See [native task IDs](docs/NATIVE_TASK_IDS.md), including the single-live-engine-per-directory and non-Linux qualification limits.

On validated Linux builds, Engine holds cooperative exclusive ownership of its data directory until workers, callbacks and storage close; a second Engine/C handle is rejected. See [directory ownership](docs/DIRECTORY_OWNERSHIP.md), especially crash-vs-durability and hostile-directory limitations.

Native `Config::journal_durability` optionally selects verified SQLite WAL/FULL; the default remains NORMAL. `ProgressEvent::journal_receipt` acknowledges only the initial task-record commit asynchronously, not source-byte persistence or completion. See [journal receipts](docs/JOURNAL_RECEIPTS.md) for delivery, failure, recovery and power-loss limits. C ABI v1 is unchanged. Rebuild C++ consumers.

C consumers may opt into `madmedia_create_v2` for NORMAL/FULL configuration and a sticky initial receipt field in poll JSON. The original v1 ABI/version query/schema remain frozen. See [C ABI](docs/C_ABI.md); a consumed terminal or explicit destroy removes the receipt from this in-memory facade.

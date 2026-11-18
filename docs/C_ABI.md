# M02a C ABI development contract

The `<madmedia/madmedia_c.h>` C99-compatible header and `MadMedia::ffi` static target provide ABI v1 development bindings. This is not a mobile package or stable 1.0 promise. `MADMEDIA_BUILD_C_ABI` defaults ON only for Linux; other targets default OFF and explicit enable fails configuration until a platform-specific entropy/lifecycle provider is implemented and verified. It does not assume /dev/urandom exists on iOS, Android or OHOS. No sibling SDK or host callback is required.

## Lifecycle

Create copies configuration strings, returns a monotonically allocated nonzero uint64 handle. Handles are not reused within a process. All entries have catch-all C++ exception boundaries. The caller must provide valid pointers/buffer lengths; these functions cannot catch invalid-memory faults.

Registry lookup obtains shared state; an operation mutex serializes API calls. Destroy atomically removes the registry entry, waits for existing operations, marks closed and joins the native engine without holding its event mutex. Calls racing with destroy either finish before close or return CLOSED; repeated destroy returns CLOSED. Destruction explicitly abandons pending snapshots. Do not treat it as completion of media tasks or as journal deletion. Use background executors because create/destroy perform I/O and joins.

No host function is called on a native callback thread. Native progress is copied into the bounded poll slots. C API currently does not expose synchronous native query, observer callbacks, recovery, pause/resume, streaming upload or credential/provider injection. Native C++ remains available separately.

## Admission, coalescing and terminals

`max_pending_tasks` is 1..256. Each admitted task reserves one slot until its terminal snapshot is copied successfully by poll. Active tasks and unconsumed terminal tasks both count against capacity. If all slots are occupied, submission returns BUSY BEFORE native submission. Each slot stores only the latest progress. Ready/Failed/Canceled is first-terminal-wins, retained and never overwritten by subsequent progress. Failed is terminal for this C façade even though native C++ supports future resume.

A poll length query or BUFFER_TOO_SMALL does not consume or release a slot. Progress may change between query and copy, so caller must retry if size grows. Poll round-robins pending slots. JSON excludes native message text/URLs/paths; byte counters are decimal strings to avoid JSON double truncation. A successful terminal copy frees admission capacity. Caller owns copied bytes, including NUL, with no library allocator crossing.

Submit always requires a task-id output buffer >= MADMEDIA_TASK_ID_CAPACITY (128) before doing work. This avoids a destructive two-call submit probe. Library generates 128-bit random-hex task IDs directly from the OS /dev/urandom source, read fully with error checks and no PRNG/counter fallback, not process-local handle counters (which would repeat across restarts). Entropy failure returns INTERNAL before slot activation/native submission; caller-provided path/task traversal through IDs is not supported by this ABI. Host-provided destination and data directory still require application sandbox validation.

## Resource failures and limits

Slots pre-reserve task ID and media ID storage. Media ID >4096 bytes or callback copy failure sets sticky INTERNAL, visible on poll and new submissions. Such callback-copy failure is not reported as delivery or a silent dropped terminal. Poll serialization allocation failure is different: it returns INTERNAL without consuming the pending snapshot, so retry is permitted. Destroy remains available; recovery/reconciliation APIs are not in this substage. No claim is made that arbitrary native allocation failures or process crashes can preserve in-memory events. Persistent journal semantics remain the native engine's responsibility.

The slot bound is not a process-wide RAM cap; native transfer/compute buffers have separate budgets. The service must obey its protocol/resource limits. Applications must use separate account-private directories and must not open concurrent engines against the same directory; Linux directory ownership locking rejects a second cooperating engine; see DIRECTORY_OWNERSHIP.md.

## Tests

- Pure C compilation/link and invalid-config/closed-handle contract.
- Submit buffer precondition, one-slot admission saturation, cancel terminal, probe retry without consumption, capacity release after terminal and concurrent poll/destroy.
- White-box 10,000 progress updates coalesce into one slot; terminal cannot be overwritten; short buffer retains terminal; oversized media ID produces sticky observable failure.
- All checks use always-active CHECK under NDEBUG.

Task IDs are collision-resistant operation identifiers, not authentication tokens or a claim that the SDK is cryptographically audited. Source reads handle EINTR, short reads and EOF/failure; there is no insecure fallback.

## Additive v2 configuration and receipt snapshots

`madmedia_create_v2(madmedia_config_v2*, out)` accepts `abi_version=2`,
`struct_size >= sizeof(madmedia_config_v2)`, both reserved fields zero, and
`journal_mode` NORMAL=0 or FULL=1. All old validation/admission constraints apply.
Invalid inputs clear a valid output pointer before returning INVALID. No new
host callback, recovery, streaming, credential or platform feature is implied.

The existing ABI v1 struct, `MADMEDIA_ABI_VERSION`, `madmedia_abi_version()` result
(1), create entrypoint and JSON schema remain frozen. Applications explicitly
link the new create_v2 symbol to opt in; do not pass a v2 struct to v1 create.
All handle operations are shared and obey the same lifecycle locking rules.

For handles created through v2, every poll JSON has numeric `journal_receipt`:
0=None/unknown, 1=initial row committed under NORMAL, 2=initial row committed
under FULL, 3=initial write failed. This field is retained monotonically across
coalesced progress and in the first pinned terminal snapshot. A successful
nonterminal poll does not clear the receipt. Size probes, undersized buffers and
JSON allocation failures do not consume it. Terminal consumption frees the slot,
so there is no later lookup or replay; reusing the slot starts at None. Conflicting
non-None receipts produce sticky INTERNAL rather than a false acknowledgment.

Receipt absence does not mean uncommitted. Shutdown abandons unconsumed snapshots
by explicit contract, and process death may occur between commit and observation.
Submission OK is still queue admission only. Receipts cover only the initial
journal task row, not source/media bytes or later state changes. FULL remains
subject to SQLite/VFS/storage behavior; see [native receipt contract](JOURNAL_RECEIPTS.md).

Regression: old v1 schema unchanged; v2 malformed size/version/reserved/mode;
short submit buffer before effects; one-slot backpressure; FULL/ NORMAL terminal
coalescing; initial insert failure; slot reuse after Failed receipt; 10,000
coalesced progress events; first terminal pin; size/short poll preservation; and
concurrent empty polling with synchronous destroy. Pure-C header compilation is
included, plus a pure-C successful FULL download/cancel/terminal receipt test.
Validated target remains Linux only, no mobile qualification claim.

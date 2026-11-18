# Later journal failures stop the task

After initial admission, every native task-record update now checks the journal
result. A failed SQLite write requests cancellation of tracked in-flight work, clears scheduled
retries and changes the **in-memory** task to Failed with error Io and fixed
message `Io: journal checkpoint failed`. The handler returns without authorizing
its next transfer/finalization/cleanup step. It does not recursively write Failed
or automatically retry a failing journal, avoiding a write-failure storm.

The initial receipt remains about the initial enqueue only. Later failures emit
receipt None; the C v2 slot can still retain its earlier initial Full/Normal
receipt alongside a Failed terminal state. That combination is not contradictory.
Callback-delivery failures remain a separate C sticky INTERNAL condition, not a
journal error. No false Ready/Paused/Canceled event is emitted for a failed target
checkpoint. A normal successful path keeps the existing events/API semantics.

The Failed state is not promised to be persisted. The journal may contain the
previous recorded state; real storage errors may have ambiguous outcomes. Use
read-only persisted reconciliation rather than treating the callback as a durable
status. Native explicit resume can retry after storage becomes usable. C facade
Failed remains terminal and has no resume/recovery API. Restart recovery follows
the persisted journal, not the previous process's transient failure state.

## Cleanup and side effects

Upload staging deletion on Ready, and upload/download staging deletion on cancel,
now occur only after the target state checkpoint succeeds. Failed checkpoints
retain those files and task context. Download cache fill is dispatched only
after the Ready checkpoint succeeds. Applications still need a retention policy
for abandoned tasks; this change does not delete old recovery evidence.

This is **not a transactional rollback** of the network or filesystem. A remote
part/upload may already have succeeded, a compute job may already have modified
staging, and a verified download may already have been renamed to its destination
before the Ready checkpoint fails. In that last case the destination is retained;
the SDK does not rename it back, delete it, or claim the previous staging still
exists. Such a stale journal may need host reconciliation on restart. No power
loss/file-fsync/whole-operation atomicity guarantee is added.

## Validation

A white-box regression stops the scheduler input thread before invoking handlers
with real SQLite, workers and callback FIFO. It injects one SQLite I/O failure at
state transitions (prepare/pause/resume/cancel/Ready), stream flush/hash, prepared
metadata, upload reconciliation, uploaded/downloaded part records, download URL
refresh and all three rewriting recovery paths. It asserts one failure/no
recursive save, fixed Io Failed event, no next control request, unchanged stored
fixture row and preserved staging (or already-renamed destination). A success
control proves Ready commit then cleanup. A separate public asynchronous test
checks runtime Failed versus persisted Queued and explicit resume after recovery.

Allocation failures, physical power cuts, and mobile platforms are not qualified
by these fault injections. Linux-only production gating remains unchanged.

### Remaining cross-attempt limitation

Cancellation of workers/HTTP is cooperative, not an immediate join per task.
Explicit resume can overlap late callbacks from the preceding attempt; events
currently have no per-attempt generation isolation. This regression does not
qualify that race or promise that old events cannot affect a resumed attempt.
Host lifecycle/retry policy must account for this outstanding reliability item.

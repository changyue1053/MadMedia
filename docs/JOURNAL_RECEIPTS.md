# Native task-record receipts (experimental)

`Config::journal_durability` defaults to `JournalDurability::Normal`. Select
`Full` to request SQLite WAL + synchronous=FULL. Startup reads back both PRAGMAs
on the actual connection and fails closed if unavailable or mismatched. Invalid
enum values are rejected. C ABI v1 remains NORMAL and has no receipt field.
C++ structs changed: rebuild all native consumers; no stable C++ binary ABI claim.

Submission success still means **command queue admission**, not persistence.
The first native event for a newly admitted task has `journal_receipt`:

- `Normal` / `Full`: the initial Queued row's SQLite autocommit returned DONE,
  after every parameter binding succeeded, under the verified selected mode.
- `Failed`: initial write failed; the event is Failed, and no commit receipt is
  claimed. Storage errors can be ambiguous; inspect the journal after restart.
- `None`: all other events, including cancellation, recovery and resumed Queued.

Receipts describe the initial task intent only. They do not snapshot or fsync
source/staging bytes, prove uploaded data exists, or promise that later state
transitions persisted. Keep the original source accessible until preparation.
Later-state journal failures stop the task with an in-memory Io Failed event;
that Failed state itself is not promised persisted. See CHECKPOINT_FAILURES.md.
No global durability promise is added by this option. Streaming remains non-resumable across restart.

## Delivery and races

The scheduler emits a receipt before processing a later cancel command. Its
single FIFO callback worker does not coalesce events; a saturated callback queue
blocks the scheduler rather than dropping a receipt. Normal destruction drains
accepted commands, then callbacks. Callbacks must return promptly, must not
destroy the Engine, and must not synchronously query it. Receipt delivery can
precede submission's return. A callback exception is contained, not retried.
Each initial enqueue emits one receipt attempt, not an exactly-once distributed
notification guarantee: process death or allocation failure can prevent delivery
although the row committed. No receipt replay occurs on recover. Absence is
**unknown**, not proof the write failed. Keep task IDs to reconcile via query.

## Limits and evidence

[SQLite's synchronous documentation](https://www.sqlite.org/pragma.html#pragma_synchronous)
explains that WAL NORMAL may lose committed transactions after power loss, while
FULL adds synchronization at commit. Actual durability depends on the VFS,
filesystem and hardware honoring synchronization. No “last transaction only”
loss bound is claimed. Performance cost is workload dependent.

The Linux regression waits for a FULL receipt, SIGKILLs a separate process,
reopens the database and recovers the record. This tests process-crash recovery,
**not power loss**. Normal/Full ordering and cancel, injected INSERT I/O failure,
invalid configuration, unavailable WAL, binding failure, IOERR/BUSY reads, and
4200 receipts across a confirmed full callback queue are tested. Journal lookup
returns NotFound only after a completed empty query; incomplete scans return Io
rather than partial success. Recovery validation waits for an actual recovered
event with receipt None. Mobile SDK/device and
power-interruption validation remain outstanding. No release is published.

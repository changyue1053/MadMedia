# Attempt epochs and safe deferred resume

Each live task context has a checked 64-bit process-local generation. Every
HTTP operation and task-producing compute result captures the generation at
dispatch. The scheduler rejects a result from a different/invalid generation
**before** changing active flags, part IDs, retry deadlines, hashes, files or state.
Cancellation, pause, failure and resume invalidate the preceding generation.
Generation overflow permanently disables resume for that context, rather than
wrapping to an old value.

Epochs are not serialized. A running context is not replaced by recover; a new
Engine only starts after the prior owner closes/joins workers and releases its
directory lock. Another process cannot have an old callback in the new Engine.
Existing journal records remain compatible. Generations are not network replay
protection or globally unique protocol request IDs.

## File I/O must finish before a new attempt

Discarding a stale event alone cannot prevent an old worker from writing over
new data. Every task-specific HTTP/compute submission also owns a completion
token retained by its task. HTTP sets completion after the transport has closed
file descriptors/cleaned up curl and its event callback returns. Compute sets
completion when the function returns, after its I/O and event posting. Control
HTTP requests are tracked and canceled too, not just data parts.

Resume invalidates old results and requests cancellation. If any prior token is
unfinished, the task remains Paused/Failed and `ProgressEvent::resume_waiting`
is true. The scheduler keeps draining events and checks completion without
blocking; only after all old I/O finishes does it emit Queued and begin another
attempt. A second pause withdraws a pending resume. Repeated pump turns do not
submit extra work or flood waiting notifications. Native C++ consumers must be
rebuilt for the new event field; the C ABI has no resume operation and is unchanged.

If an OS call/worker never returns, resume remains waiting rather than risking
file corruption. This is not a timeout-based forced resume. Resource tokens stay
bounded by outstanding work; old completed tokens are pruned. Graceful Engine
close still joins work and can wait for an unresponsive syscall. Host callbacks
must return promptly and must not destroy the Engine or synchronously wait on
it. No scheduler lock is held while waiting for completion.

Stream chunks are kept buffered until their flush metadata checkpoint succeeds.
A stale/canceled flush cannot consume the next attempt's buffer. On checkpoint
failure, its speculative size/bitmap/count are rolled back in memory so an
explicit later retry uses the same offset, after old I/O is quiescent.

## Two-phase cancellation

Cancel first records an in-memory intent, invalidates the epoch and requests
cancellation of all tracked work. It retains the task, tokens and staging while
any old I/O can still write. `ProgressEvent::cancel_waiting` marks this interval;
repeated cancel is idempotent, and resume/pause cannot override the intent. Pump
will not dispatch new work for this task. The scheduler keeps draining, never
synchronously waits for a worker that might be posting to its queue.

Once quiescent, it commits Canceled, attempts staging unlink and emits exactly
one terminal event, then releases the context. The C terminal slot remains
occupied until this event is successfully polled. If the checkpoint fails,
intent is cleared, files/context are retained and a runtime Io Failed event is
emitted; no automatic write retry. If unlink fails (other than missing file),
Canceled carries error Io and fixed native message `cancel cleanup failed`.
The task ended, but cleanup did not complete. This diagnostic is observable in
native callbacks and C poll's error field; it is not an extra durable journal
annotation. Explicit close abandons unconsumed snapshots as before.

Close first drains the command queue and joins HTTP/compute, then finalizes
quiescent pending cancels before draining callbacks. It can still wait forever
for a stuck syscall; no premature terminal is claimed. Cancel intent itself is
not persisted while waiting. A crash before the Canceled commit can leave the
old recoverable journal row; a host requiring crash-durable cancellation intent
must not infer it from admission or a waiting event. Other-task path conflicts
and remote side effects remain outside this transaction boundary.

## Timers, retries and remaining scope

Retry deadlines are scheduler fields, not external timer callbacks. Invalidating
an attempt clears these fields; old responses cannot re-arm them because the
epoch check runs first. Automatic retries within one attempt follow the existing
transport's one-terminal-callback-per-request contract. Completed immutable cache
fill is post-Ready background work, not a task result producer or resume writer.

This isolates successive attempts of the **same task**. It does not serialize
unrelated tasks that a host directs to the same destination path, undo remote
side effects, force a stuck I/O operation to finish, or add whole-file durability.
Applications must avoid conflicting destinations across tasks. Cancel cleanup
is not a general filesystem transaction; the staging/rename caveats in
CHECKPOINT_FAILURES.md still apply. No mobile qualification is implied.

## Regression evidence

The deterministic probes stop the scheduler input thread and invoke its real
handlers/pump directly while real compute/HTTP workers run. They prove the
controlled ordering cases, not exhaustive concurrent scheduling or device stress
qualification. The existing public API end-to-end suite is also retained.

Tests reject all seven compute/HTTP event categories (all eight HTTP operation
kinds) before current flags change; cover repeat recover and generation overflow;
clear retry deadlines; prove no new work during 100 waiting pump turns; withdraw
and re-request resume; and verify stream buffer retention. A delayed compute
worker actually writes old staging bytes before release, then resumed preparation
writes new bytes. A real loopback HTTP download blocks inside pwrite while pause/
resume occurs: the second write begins only after the old transport closes its
I/O and completion token finishes. Late old responses cannot commit a bitmap.
Close waits for a held worker and finishes after release without lock deadlock.
The audited stream flush blocked after its cancel check but before O_CREAT is
covered: pending Cancel retains staging, then removes it only after the writer
returns, including close-while-pending. Blocked HTTP pwrite plus queued control
HTTP, checkpoint failure, unlink failure and a public C capacity-one terminal
slot exercise the same two-phase contract.

## Streaming resume and process recovery

Within-process resume preserves the locally checkpointed part count and byte
extent. A streaming init response with a null/zero total must not reset them.
If a session already exists, its identity must match; once local parts exist,
part size must also match, and server acknowledgement indices must lie within
the local extent. Conflicts fail closed with fixed Protocol diagnostics rather
than overwriting local state or reusing an incompatible offset.

Recording buffers and recording completion intent are not durably reconstructible
across process death. Recovered streaming tasks therefore cannot resume: explicit
resume reports Failed/State with `State: streaming task cannot resume after restart`.
They can be queried or canceled. This is intentionally not a durable recording
resume feature. Ordinary non-streaming recovery retains its existing contract.

The public API regression uploads distinct bytes before and after pause/resume,
downloads the completed object, and checks exact concatenation, both normally and
with an injected connection drop. A child process is SIGKILLed after checkpointed
stream parts; recovery and explicit resume rejection, then cancellation, are
verified. White-box cases reject changed session, changed part size and an
out-of-range acknowledgement while preserving local extent.

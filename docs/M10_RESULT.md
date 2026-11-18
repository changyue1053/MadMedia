# M10: generations plus I/O quiescence before resume

Task-specific compute/HTTP events capture a process-local epoch. Scheduler
handlers reject stale epochs before current-state mutation. Pausing, failing,
canceling and resuming invalidate preceding results; overflow is fail-closed.
All task work, including control HTTP, retains completion tokens. Resume remains
pending with a native `resume_waiting` indication until old I/O truly ends, rather
than merely dropping its result. New work cannot overlap prior-attempt writes.

Completion is marked after HTTP descriptor cleanup and callback return, or
compute function return. Scheduler waiting is nonblocking, so a worker blocked
posting to its bounded queue can still drain. Unresponsive syscalls can keep
resume/close waiting; no unsafe timeout-based forced start. A second pause can
withdraw deferred resume from either Paused or Failed. Stream chunks remain
buffered until a successful current-generation metadata commit.

Independent review found a real Cancel race: a stream writer blocked after its
cancel check recreated staging after premature cleanup. Cancel is now two-phase,
retaining context/files until quiescence, then checkpoint/cleanup/one terminal.
Tests incorporate that exact O_CREAT timing, HTTP pwrite plus queued control,
close-while-pending, checkpoint/unlink errors, and public C terminal-slot pressure. Deterministic
white-box probes cover all compute/HTTP result types, retry deadlines, overflow,
repeat recovery, deferred resume/withdrawal, stream data, and close. They stop the
input scheduler and invoke real handlers/pump while actual workers run. A held
compute writer and a loopback HTTP download blocked in pwrite verify old-write
completion precedes resumed writes. These are controlled proofs, not exhaustive
concurrency or device-stress qualification. Existing public e2e tests remain.

A real streaming regression also found that init responses with null totals
could reset local part count and overwrite the beginning of a recording. Resume
now preserves local extent, validates session/part geometry and acknowledgement
bounds, and rejects recovered streaming resume after process death because its
recording buffer is not reconstructible. Public tests verify exact bytes with
normal/injected-disconnect transport and SIGKILL recovery rejection.

Final full matrix and remote verification appear in the external publication
record. No dependency/source-license changes or release. C++ consumers must be
rebuilt for the event field. No C resume API or mobile qualification is added.
Unrelated tasks sharing destinations are not serialized; cancellation is not a
filesystem transaction and remote effects are not rolled back. See
ATTEMPT_ISOLATION.md and CHECKPOINT_FAILURES.md.

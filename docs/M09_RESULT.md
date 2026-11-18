# M09: fail-closed later journal checkpoints

All post-admission record writes now check the SQLite result. Failure produces
one fixed in-memory Io Failed notification, requests cancellation of tracked
work, clears retries, and stops the current handler without recursive Failed
writes. Initial enqueue receipts retain their existing meaning. Ready/cancel
cleanup and download cache fill follow successful target checkpoints.

17 deterministic handler scenarios cover transitions, metadata, streaming,
reconciliation, parts, credentials and recovery rewriting, using real SQLite,
workers and callbacks with the input scheduler stopped before direct invocation.
A public asynchronous cancel-failure/manual-resume case checks persisted/runtime
state separation. A success control proves Ready commit precedes cleanup.
Independent review passed the related tests with no new blocker.

Installed C/C++ consumers pass. Final full matrix and remote blob verification
are in the external publication record. No third-party source/dependency or
release. ASan disables LeakSanitizer under the existing sandbox limit.

Prior network/file effects are not rolled back. In particular a download rename
may precede the failed Ready checkpoint, leaving the destination present and
journal stale. Failed is not itself guaranteed persisted. Cross-attempt late
callbacks after explicit resume remain unqualified: there is no generation
isolation yet. See CHECKPOINT_FAILURES.md for the precise contract.

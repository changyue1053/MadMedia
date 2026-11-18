# M08: read-only persisted-record reconciliation

Adds native `query_persisted` and C `madmedia_query_persisted` for journal-only
reads after unknown/lost receipt observation, including after process restart.
No live-map fallback, recovery, pending-slot admission, event consumption, sync
mode inference, or state mutation. C input IDs are bounded ASCII; output omits
paths and raw error strings. NOT_FOUND is additive status 8, preserving 0..7.

Scheduler record-copy exceptions are propagated through the query promise rather
than terminating the scheduler or stranding the waiting caller. C++ allocation
errors return INTERNAL; SQLite errors remain NATIVE_ERROR, never NOT_FOUND.

Tests cover old-row reopen on both C configs; no implicit recovery; input bounds;
private-field omission; SQLite IOERR/BUSY/NOMEM; injected scheduler bad_alloc and
successful retry; probe/copy growth; invalid stored enum; concurrent close; and
native live-memory/journal disagreement. The pure-C FULL test now invokes the
persisted query from compiled C. Installed C/C++ and an old-header C consumer
linked against new libraries pass.

Full matrix outcomes and remote verification are recorded in the external
publication record. Linux-only qualification and disabled LeakSanitizer limits
remain. No release, new dependency or third-party source. A row is not proof of
power-safe persistence; NOT_FOUND is not proof of never committed. See C_ABI.md.

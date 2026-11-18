# M07: additive C configuration v2

Adds a separate `madmedia_create_v2` symbol and versioned config for optional
WAL/FULL journal mode. v2-only JSON includes an initial receipt snapshot retained
through coalescing and terminal pinning. v1 struct, version result, create call
and JSON schema stay unchanged. No new third-party code or release.

Verified regressions include malformed size/version/reserved/mode; submission
buffer preflight; capacity-one backpressure; FULL and NORMAL terminal receipts;
SQLite trigger-injected initial write failure then slot reuse; 10,000 progress
coalescings; progress poll preserving the receipt; repeated identical receipt;
conflicting receipt failing closed; probe/short buffer; and concurrent poll/close.
The previous commit's exact v1 header and C consumer were compiled and linked
against the new static libraries successfully. Installed current C (v2 symbol)
and C++ consumers also pass. Independent review found no blocker and its pure-C
FULL download/cancel/terminal-receipt probe is now an automated CTest, with
private mkdtemp fixtures and exact-path cleanup after native shutdown.

The final publication record records complete build matrix outcomes. ASan/UBSan
uses leak detection disabled due to sandbox limits. C++ source ABI remains
experimental. Linux qualification only; no iOS/Android/OHOS support claim.

Receipt absence or loss at explicit close/process death is not evidence of a
missing commit. This API neither recovers old in-memory snapshots nor guarantees
source-file durability. See C_ABI.md and JOURNAL_RECEIPTS.md.

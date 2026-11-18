# Native task-ID hardening

Native C++ default upload/download/stream IDs and the C ABI now share the reviewed Linux OS entropy provider. It reads all 16 bytes with EINTR/short-read checks; no single-seed PRNG, clock or counter fallback remains. These are collision-resistant operation IDs, not authentication tokens. Prefixes remain t_/d_/v_ for native defaults and madmedia_ for C ABI compatibility.

Entropy failure returns native Err::Busy from upload/download/stream_begin before admission (C ABI entropy failure remains INTERNAL). It causes no enqueue, callback or journal row. Native helper gen_id throws on failure; it is an internal utility, not the error-returning public Engine API. Without the CMake Linux-validated provider definition the shared default entropy port fails closed (Android's __linux__ macro is not used as qualification) pending target-specific provider qualification. Apple/Android/HarmonyOS candidates remain host-model only; neither native default-ID generation nor mobile C ABI support is claimed there.

## Collision and admission contract

Engine startup reads a dedicated task-ID inventory from its journal and fails if the query cannot complete. It does not load whole task records merely to reserve IDs. A mutex-protected set reserves IDs before queue admission. Duplicate provided IDs, injected random collisions and IDs from a prior process are rejected with Err::State before enqueue. New explicit IDs must be 1..128 ASCII letters/digits/underscore/hyphen, excluding path traversal. Persisted legacy IDs are not renamed; recovery/resume retain existing data.

An admission guard releases a new reservation if record construction, return-value preparation or queue insertion fails. Return-value allocation occurs before enqueue. Once accepted, the ID stays reserved for the engine lifetime, even after completion/cancel or initial journal failure. This conservative policy prevents reuse while a terminal event is pending; memory scales with persisted+accepted task count and is not globally bounded. A queue rejection may be retried with the same ID. Recovery uses already reserved persisted IDs without creating new admissions.

Queue acceptance is not a durability acknowledgment. The scheduler saves the initial journal row before transfer; initial save failure emits Failed and retains the in-process reservation. Crash before persistence can lose an accepted queued task; no durable-submission guarantee is added. After restart, only durable rows populate the inventory. Supplied IDs for never-persisted tasks may be admitted again; new default IDs draw fresh OS entropy.

This is NOT a cross-process lock. Two live engines must never share a data directory, and external edits/deletes to an active journal are unsupported. Linux cooperating engines now also use directory ownership locking described in DIRECTORY_OWNERSHIP.md. The in-memory set itself is not a cross-process allocator, and durable submission remains a separate gate. This stage does not claim SQLite transaction bridging with MadDB.

## Tests

Native entropy open failure, short read/EINTR, EOF and EIO; all three native submit APIs fail before admission on entropy failure. Fixed entropy causes a deliberate duplicate default ID and is rejected. Twelve concurrent callers for one explicit ID admit exactly one. A reopened engine rejects a persisted explicit ID and deterministic random collision. Two independent processes create two distinct defaults, two journal rows and two staging paths while rejecting the first process's ID in the second. C ABI regressions run unchanged against the shared provider.

Fault injection tracks descriptor close as well as open/read/fortified __read_chk, so a recycled descriptor cannot accidentally intercept later file I/O.

A deliberate scheduler gate fills the real command queue, rejects an upload and then verifies that the same ID is admitted after draining. A separate sqlite3_step IOERR injection proves startup cannot accept a partial ID inventory. These are test-only injection hooks, not production callback behavior.

The build gate is explicit and additionally rejects Android/OHOS/OpenHarmony compiler macros even if a Linux qualification define is supplied accidentally. Four host-model tests cover an absent qualification define and three simulated mobile macros with that define. They prove fail-closed branching, not mobile SDK compilation or device functionality.

Final Linux verification: Release with optional MadObs 36/36, ASan+UBSan configuration 35/35 (leak detection disabled), explicit FORTIFY3 Release 35/35. Installed C ABI and C++ consumers passed. Independent review confirmed collision/admission behavior and the explicit mobile exclusion gate; no target-device validation or release is implied.

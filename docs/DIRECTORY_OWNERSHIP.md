# Single live owner per data directory (Linux development contract)

Engine::start obtains a nonblocking exclusive flock on `data_dir/.madmedia.lock` before opening the journal or creating staging/cache contents. A second cooperating engine returns Err::Busy, including independent opens in one process, path aliases and separate processes. C ABI creation reports NATIVE_ERROR for native ownership failure and leaves its output handle zero. Caller may retry after the first Engine/C handle is fully destroyed.

The descriptor uses O_CLOEXEC/O_NOFOLLOW/O_NONBLOCK. Only a regular single-link lock file is accepted. The SDK neither truncates nor unlinks it, including on shutdown, because replacing its inode could defeat mutual exclusion. The empty file remaining after shutdown is expected, not a stale lock to delete. Ownership is released by closing the descriptor, including kernel cleanup after process termination.

The lock is declared first in Engine::Impl and therefore destroyed last, after native workers/callbacks/storage. Initialization failures release it via RAII. Callback-drain tests verify that attempts to open the same directory during shutdown, including a reentrant attempt from a finishing callback, remain rejected until the old Engine is gone. This is not permission to destroy or recursively call the existing Engine from its callbacks; the normal native lifecycle contract still applies.

## Boundaries

This is advisory cooperative locking on tested Linux local filesystems. The host must own and control the data directory. It does not defend against a user/process with write permission deleting/replacing the lock file or renaming/replacing the active data directory. Do not edit the active journal externally. No network-filesystem, sandbox-migration, bind-mount replacement or hostile-local-process guarantee is made. Different application/account directories remain the recommended isolation boundary.

A process crash releasing flock does NOT mean queued work was durably committed. Existing SQLite WAL policy and recovery semantics are unchanged. Queue success remains admission, not a durable commit acknowledgment. Restart must inspect/recover journal state separately. Directory ownership prevents cooperating simultaneous writers; it is not a MadDB transaction bridge or a general cross-process API.

Non-Linux ownership providers are unqualified and fail closed; Android/OHOS/OpenHarmony compiler macros cannot implicitly enable the Linux provider. Mobile toolchains and lifecycle/device tests are still pending. Fork without exec is not a supported way to transfer an Engine or ownership descriptor; fork+exec children drop the inherited close-on-exec descriptor.

## Verification

Same-process distinct descriptors, dot-path alias, directory symlink alias and independent processes are rejected while one owner is active. Different directories open independently. Normal close and SIGKILL recovery allow reopening. Symlink/FIFO lock entries are refused without altering a sentinel. Failed journal initialization does not leak ownership. C ABI duplicate-open rejection and synchronous destroy/reopen pass. Callback shutdown/reentrant-open tests keep the lock until callback draining finishes. These tests exercise real flock, not a mock filesystem.

An independent OOM probe found an initialization transfer-order defect: moving ownership into Impl before a throwing config copy could release it ahead of a still-local journal. Config copy now precedes both noexcept ownership transfers. A targeted bad_alloc regression intercepts sqlite3_close and verifies that acquiring the directory still returns Busy at that moment, then confirms reopening after cleanup. The original independent reproducer now reports the lock is not free before sqlite close.

Final Linux matrix: Release with MadObs 38/38; ASan+UBSan configuration 37/37 with leak detection disabled; explicit FORTIFY3 Release 37/37. All seven thread-initialization fault cases additionally reopen the same directory after failure. Unqualified/mobile-macro host models reject the lock provider. These counts do not establish mobile target support or power-loss durability.

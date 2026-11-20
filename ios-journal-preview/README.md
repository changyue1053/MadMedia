# Test-only installed iOS Journal preview

This separate checkpoint packages only the existing C++ Journal translation
unit and existing standalone Apple entropy candidate; it does not build or
start the engine, enable a C ABI, build Curl or a downloader, or perform
transfer jobs. The task record is synthetic persisted data, not an admitted
task. SQLite and Security are the public frameworks/libraries of the pinned
Xcode/iOS Simulator. No production build or SDK/API change.

A disposable producer compiles the fixed source set, checks platform and
ARM64, installs one archive and its needed headers, relocates them into an
independent app and deletes the producer and build directories. The app links
that exact archive by the same explicit installed path pattern verified by
the ImageIO preview, and must link system SQLite and Security. No source
translation unit from the producer is compiled into the consumer.

The first named XCTest uses the production Journal, which verifies WAL/FULL
when opening, writes one transaction via its normal autocommit upsert, closes,
reopens and verifies, updates and reopens, deletes and reopens, then removes
only its app-private disposable directory. This proves ordinary close/reopen
behavior, not crash/power-loss durability, concurrent writers or a transfer.
The other XCTest exercises the unmodified Apple entropy candidate twice,
rejecting simple all-zero or repeated output; not entropy certification.
Production DirectoryLock on this exact source remains Linux-only and is
intentionally omitted. The separate Apple lease fixture is not relabeled.

Exact workflow trigger: `checkpoint/madmedia-ios-journal-preview`. Xcode
26.2/17C52, official SDK/runtime 26.2, stock ARM64; no physical device, raw
transcript or artifacts. Native execution is not implied by portable tests.

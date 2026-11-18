# M06: opt-in native journal receipts

Scope: Linux-native initial task-record persistence contract. Default NORMAL and
C ABI v1 admission semantics are unchanged. FULL verifies actual WAL/synchronous
settings; new source-level native receipt is attached only to initial enqueue.
No release, mobile qualification or media-file durability claim.

Independent review identified lookup IOERR incorrectly reported as NotFound and
partial scan success. Fixed with binding/step checks and RAII; reviewer original
probe now reports `load_ok=0 error=Io unfinished_ok=0 unfinished_size=0`.

Regression coverage: NORMAL/FULL first receipt; cancel ordering/no duplicate;
INSERT I/O and parameter-binding failure; invalid mode/unavailable WAL; initial
and partial-read IOERR/BUSY; 4200 receipts across a verified saturated FIFO;
separate-process SIGKILL after receipt, persisted record and actual recovery
callback with no receipt replay. Saturation's bounded wait is 15 seconds because
ASan completed only 3026 commits in the original 5-second test budget. It still
requires precisely 4098 commits while the callback is blocked, and all 4200
receipts after release/drain. CTest timeout is 45 seconds.

Build resource note: initial ASan/FORTIFY compilation failed explicitly writing
compiler temporary assembly to a full `/tmp`. Rebuilt with temporary output on
the shared workspace; no production workaround or hardening reduction.

Full final matrix is recorded in the publication record outside the source tree.
LeakSanitizer remains unavailable in this sandbox; ASan/UBSan runs disable leak
detection, so no leak-clean claim. Installed pure-C and C++ consumers pass.

No new third-party source or license terms were introduced. First-party license
choice remains pending as documented in the existing notices. No secrets found
in changed source scans. See JOURNAL_RECEIPTS.md for all durability limitations.

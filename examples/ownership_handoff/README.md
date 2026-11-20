# Installed Linux ownership handoff

This package check complements the sequential two-app gate. The existing core
and C ABI are built with tests, tools and optional MadObs disabled in a clean
disposable source snapshot. The installed prefix is relocated and the producer
source, build tree and old prefix are gone before two separate CMake consumers
are configured and linked from the installed package. CMake verifies the
imported archives point to the relocated Release installation.

A pure C executable opens a fresh Linux-owned data directory using the v2 FULL
configuration, observes no pending event and no row for a synthetic missing ID,
then holds the handle live. An independent C++ process must get `Err::Busy` on
the same directory but can hold a second fresh directory. After the C owner
receives a release command, explicit destruction must return `MADMEDIA_OK` and
a second destroy must return `MADMEDIA_CLOSED`. The C++ consumer then opens the
first directory while maintaining ownership of the second; the C process
acknowledges a final command only afterward, so process exit did not release the
first lock. The C++ consumer checks the read-only missing row, idle callback
health and no callbacks, and closes on its owning thread.

Only fixed, bounded pipe synchronization and job-owned data directories are
used. No task, recovery, transfer, provider or network request is configured
or submitted. The idle constructor and the original package dependency graph
remain unchanged, including system CURL, SQLite and Threads. No dependencies
are installed by this gate. If the stock runner lacks a dependency it fails,
not skips. This is not a cloud/media transfer, persistence, power-loss,
crash-recovery, arbitrary cross-process safety, or mobile-device claim.

Run from a clean Linux checkout with Python 3.9+ / pidfd support, the usual
CMake C/C++ tools and already installed stock development dependencies:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 scripts/test_installed_ownership.py
PYTHONDONTWRITEBYTECODE=1 python3 scripts/verify_installed_ownership.py --sha "$(git rev-parse HEAD)"
```

The first command tests the pipe supervisor with synthetic executable fixtures;
it is not evidence about MadMedia.

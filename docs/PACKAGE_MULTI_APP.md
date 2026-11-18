# Installed package and two-app gate (M13)

This gate is about independently consuming an installed native SDK, not adding
more runtime APIs. It builds a disposable source copy, installs the SDK, moves
the install prefix, then removes that disposable producer source and build tree
before configuring consumers. The real checkout is never moved or deleted.
Only files tracked by Git (including staged additions locally) enter that copy.

Two separate executable processes consume the relocated package:
- App A uses the C++ facade, journal reconciliation and callback health.
- App B compiles a pure-C source against C ABI v2/FULL, polls its terminal and
  queries its persisted row. The final link still needs the C++ runtime/toolchain.

Each app uses a different data directory and generates its own task ID. Both
persist their own row, close, then reopen their own directory and assert that
the other app's ID is NOT_FOUND. The processes run sequentially; this is not a
concurrent-app scheduling/stress test, an OS privacy sandbox test or a shared
storage entitlement. Apps intentionally sharing a data directory remain subject
to the cooperative ownership lock and are not supported as concurrent owners.
The probes use a local nonservice endpoint plus cancellation, not an end-to-end
remote media delivery test. Full transfer regressions remain in the native suite.

## Optional MadObs contract

Both Obs OFF and ON consumers are tested. OFF needs no MadObs checkout or runtime.
ON consumes the installed `MadMedia::obs` component and a pinned public MadObs
observer header, supplies a host-owned callback sink and checks actual finite
attribute emissions. It does not link or qualify MadObs runtime ingestion,
persistence or export. The pinned reference is
`e3320ed5fc3ea03171dd5dab8231a8e7f569a983` in `changyue1053/MadObs`;
observer.h blob `2fd8c8cc81b2e38569c2b2ce3cd5fec03ffcf001`.
No external source is vendored into this repository and its license notices are
unchanged. No first-party open-source license is invented by this gate.

## Toolchain scope and evidence

The CI job fixes `ubuntu-24.04` and compiler majors GCC 13 / Clang 18, in both
producer→consumer directions and with Obs OFF/ON (four cells). These packages
are provided by Ubuntu's repositories: [GCC 13](https://packages.ubuntu.com/noble/amd64/g%2B%2B-13)
and [Clang 18](https://packages.ubuntu.com/noble/amd64/clang-18). Patch versions and system
dependencies may receive updates; the exact `--version` output and produced
static archive hashes are recorded in each cell's evidence artifact/log. This
is not a bit-reproducible toolchain image or libc++ interoperability claim.

Local preparation used Debian GCC 14.2 in both directions because Clang was not
installed. Local Obs OFF/ON results are distinct from the cross-compiler CI
results. Do not claim GCC↔Clang success until the exact commit's four CI cells
pass. This adds no Android, Apple, HarmonyOS, architecture-slice, device or app
background-lifecycle qualification. It is not a production release.

Run the gate with explicit producer and consumer compilers:

```sh
python3 scripts/verify_package.py \
  --producer-c gcc-13 --producer-cxx g++-13 \
  --consumer-c clang-18 --consumer-cxx clang++-18 \
  --obs OFF --evidence package-evidence.json
```

For Obs ON, add `--obs ON --madobs-include /path/to/pinned/MadObs/include`.
Use an absolute TMPDIR on a volume with sufficient capacity. Each run removes
only its freshly created temporary directory; persistent evidence is written to
the caller-selected evidence path, and command diagnostics remain in the log.
Additional dependency locations can be supplied using repeatable
`--cmake-arg=-DName=value` arguments for both producer and consumer. Accepted
names are SQLite3_INCLUDE_DIR, SQLite3_LIBRARY, CURL_INCLUDE_DIR,
CURL_LIBRARY_RELEASE and CURL_LIBRARY_DEBUG; gate/toolchain overrides are rejected.

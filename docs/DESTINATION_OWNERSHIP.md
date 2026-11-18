# Download destination admission (M11)

This is a cooperative **single Engine** ownership rule, not a cross-process file
lock or a filesystem security sandbox. Hosts must use stable, case-sensitive
local paths. Hosts MUST NOT supply external bind-mount aliases of any descendant
of Engine-private storage, even if the mount predates Engine startup and remains
stable. Hosts must not replace parents, mounts, links or output files while
an Engine is alive. Different Engines/data directories and external writers do
not coordinate through this registry. Case-folding filesystems are not qualified.

## Protected write set

A download owns three paths: its destination, `destination.part` (transfer
staging), and `destination.cachetmp` (post-Ready cache population). Any overlap
between any of these paths and another owner's write set rejects admission.
Cache population may still be reading/writing after Ready, and Failed tasks may
resume. Therefore **all accepted reservations last until Engine destruction**,
even after Ready, Failed, Canceled, callback delivery or C terminal consumption.
HTTP/compute workers, including cache population, are joined before destruction.
A second download to the same output must use another path or wait for a safely
closed/reopened Engine. There is intentionally no early-release API.

There are at most 4096 download owners per Engine, including restored unfinished
rows and completed tasks from this lifetime. The 4097th admission returns Busy.
This limit is independent of active concurrency and the C facade's pending-event
limit. It is not a bound on all native task history/IDs or journal size.

## Paths and aliases

Fresh relative paths resolve against the calling process's current directory at
admission. The parent directory must exist and is canonicalized. The stored and
used output is absolute, so later cwd changes cannot redirect it. Empty names,
`.`/`..`, embedded NUL, leaf symlinks (including dangling links), nonregular
existing leaves and multiply-linked regular files are rejected for all three
paths. Missing leaves are allowed; admission does not create or truncate them.

Ownership keys use parent device/inode plus basename, covering ordinary parent
symlink and bind aliases on qualified case-sensitive filesystems. Existing leaf
inodes are also compared. This does not make path checking atomic with future
file opens: concurrent external topology changes are outside the contract.

The Engine's staging and cache directory subtrees are forbidden destinations,
including ordinary symlink aliases and direct root aliases identified by ancestor
device/inode. This does NOT detect an external bind of a deeper private descendant:
that mount hides the private ancestor from traversal. Such inputs are outside the
trusted-host contract, not a protected case. Its journal.db,
journal.db-wal, journal.db-shm, journal.db-journal and .madmedia.lock are also
protected, including existing inode aliases. Component boundaries matter:
`staging-other` is not the `staging` subtree. A normal file such as
`data_dir/output.bin` remains allowed; the entire data directory is not a sandbox.
Hosts must still avoid unrelated application inputs and other Engine storage.

## Admission, errors and recovery

Path inspection happens before taking the ownership mutex. All three keys are
accepted atomically with one owner insertion. Queue rejection or an exception
before successful enqueue rolls back the owner and task-ID admission. After
queue acceptance, even an initial journal write failure retains ownership for
this Engine lifetime. Conflicts and invalid paths return State; capacity returns
Busy in the native API; the C submission facade retains its existing
`MADMEDIA_NATIVE_ERROR` mapping for native rejection. No path or credential is included in registry diagnostics.

Before exposing an Engine, a complete successful unfinished-row scan reserves
all download outputs, even if the host never invokes recover. IOERR/BUSY or an
incomplete scan fails startup. Conflicting legacy rows, excess owners, missing
parents or now-unsafe leaves also fail startup rather than choosing a winner or
silently skipping a row. This fail-closed behavior can reduce availability.

Legacy relative destination rows are rejected because their original cwd cannot
be recovered from the journal. They require a separately reviewed migration with
the correct original directory; this SDK does not guess, delete or rewrite them.
A nonempty historical staging path must equal its recorded destination plus
`.part`. Recovery uses the same reserved canonical destination and corresponding
staging path, not the original alias. Repeated recover does not reserve twice.
Ready/Canceled rows are not restored as owners in a new Engine, after all old
workers have been joined. This does not promise cleanup of crash-left artifacts,
whole-file durability, or protection from external processes.

## Verification scope

The regression covers three-path cross-collisions, parent symlink and dot aliases,
leaf symlink/hardlink rejection, protected internal paths, concurrent admissions,
4096-task capacity and startup excess, canceled-task retention, queue rollback,
absolute recovery and duplicate legacy rows, invalid relative/staging history,
SQLite inventory IOERR/BUSY and cwd changes after admission. The optional destination_bind_boundary.cpp probe documents a negative boundary:
with `data/cache/sub` bind-mounted at an external `alias`, `alias/file` can be
accepted. This is evidence of an unsupported input, not a security test pass.
Case-folding filesystems are likewise not runtime-qualified by this host suite.

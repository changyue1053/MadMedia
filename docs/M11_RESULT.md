# M11: bounded download destination ownership

Downloads now reserve destination, transfer staging and post-Ready cache-temp
paths together. Parent identity and canonical paths prevent ordinary alias
collisions. Existing unsafe leaves and SDK-private storage destinations are
rejected. Reservations remain through Engine shutdown, including terminal tasks,
with a 4096-download lifetime limit. Failed queue admission rolls back all keys.

Private descendant bind-mount aliases remain explicitly forbidden host inputs:
review reproduced their acceptance because the mount hides the private ancestor.
This cooperative registry is not a security boundary against those paths.

A complete unfinished journal inventory is required at startup; collisions,
unsafe legacy paths and excess owners fail closed. Recovery uses the reserved
canonical paths. Relative legacy destinations require an explicit migration,
not a guessed cwd. No journal rows are automatically removed or repaired.

See DESTINATION_OWNERSHIP.md for exact compatibility, scope and availability
costs. This is not a cross-Engine lock, hostile-path sandbox, mobile qualification
or release. Final test/review/publication evidence is recorded separately.

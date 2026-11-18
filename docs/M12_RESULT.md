# M12: observable native host callback faults

The prior worker and facade already contained callback exceptions but silently
swallowed them. M12 adds a coherent nonblocking health snapshot with invocation
count and sticky source/terminal/receipt flags. Observer faults still do not
prevent business callbacks; exceptions never escape through a new marker.
Only one fixed diagnostic is logged per Engine, without exception contents.

The counter acknowledges a faulted invocation, not lack of delivery or commit.
There is no retry or per-task replay. Blocking/self-destroying callbacks remain
unsupported, and callers must keep Engine lifetime valid during health reads.
No C ABI or ProgressEvent layout is changed. See CALLBACK_HEALTH.md for exact
TLS, saturation, memory-order and lifetime semantics. Final review, test matrix,
TSan availability and remote verification are recorded separately. No release.

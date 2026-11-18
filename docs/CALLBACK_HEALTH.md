# Native callback health (M12)

`Engine::callback_health() const noexcept` is an allocation-free, nonblocking
snapshot for a **live** Engine, including when called on its callback thread.
`madmedia::CallbackHealth` is an alias for the native result. No ProgressEvent,
C v1/v2 config, C version query or poll JSON layout changes. There is no new C
health API: the C facade retains its existing sticky callback-copy error path.
Native consumers must rebuild/link the updated core for the additive method.

## Meaning of the snapshot

- `faulted_invocations`: the number of notification invocations that observed one
  or more host exceptions, saturated at `2^60 - 1`. Observer and business callback
  both throwing for one notification counts **one**, not two. Successful later
  invocations never reset it.
- `callback_fault`: at least one business ProgressCallback invocation threw.
- `observer_fault`: at least one optional facade ObserverPort invocation threw.
- `terminal_callback_fault`: a business callback threw while handling a Ready,
  Failed or Canceled notification. Observer-only faults do not set this bit.
- `receipt_callback_fault`: a business callback threw while handling a non-None
  initial journal-receipt notification, including a failed receipt.

The four booleans are sticky until destruction, including after counter
saturation. They describe callback execution outcomes, NOT native task failure,
no commit, no delivery, or an exactly-once acknowledgement. A callback might
already have processed the event and then thrown. Delivery is therefore unknown;
there is no automatic retry, which could duplicate application side effects.
Query native/persisted task state from another executor when reconciliation is
needed. Health has no task IDs, event history, exception text, timestamps or
per-task replay. It cannot identify which terminal or receipt faulted.

The snapshot uses one lock-free 64-bit atomic: high 60 bits for count, low four
for sticky flags. The build requires lock-free uint64 atomics. An update is
published with release semantics and read with acquire semantics, providing a
coherent health value, not a substitute for task scheduling/receipt ordering or
synchronization of unrelated host data. A callback reading health while executing
cannot see an exception it has not yet thrown. Fault publication occurs after
the invocation returns/throws. Hosts must poll or otherwise inspect health; a
second callback is not used to report failures of the callback itself.

## Containment, source and logging

The facade still invokes the observer before the business callback. Observer
exceptions are contained and do not prevent the business callback. Both catches
report only a source bit through an internal noexcept function into the current
callback-thread TLS notification scope. Nested scopes restore their predecessor;
different Engine callback threads do not share the context. A native callback
that throws directly is classified as a business fault. A nested direct call to
`observe(...)` during one notification contributes to that notification's source
flags. Direct calls outside an Engine notification still contain exceptions but
have no Engine health sink and are not counted. No new exception marker crosses
an API boundary.

The first fault per Engine emits the fixed diagnostic
`host callback fault; inspect callback health`. Later faults update health without
repeating the log. Exception `what()` strings or objects are never logged or
retained. User callback code must itself avoid logging sensitive data.

## Lifetime and remaining failure modes

Host callbacks/observers must return promptly. A never-returning host function
still blocks subsequent notifications and can block close forever; health cannot
detect future completion or forcibly unwind it. Destruction, unique_ptr reset or
release-and-delete of the Engine from its own callback remains forbidden because
shutdown joins that callback worker. This change does not implement safe
self-destruction, detach workers, leak the Engine or transfer ownership silently.

The health atomic is owned by the implementation and outlives the callback
worker, so a callback being drained during close can still read it without
racing callback-worker ownership reset. `callback_health()` does not otherwise
keep the Engine alive: external callers must prevent their calls racing
destruction, just as for other native methods. Synchronous query
from the callback remains rejected; health is the separate nonblocking API.
Queue construction/copy allocation failures before a callback invocation are not
host callback throws and are outside this counter's coverage. Signals, abort,
process termination and arbitrary host memory corruption are not contained.

## Evidence scope

The test checks native and facade callback exceptions, observer-only and dual
exceptions, exact once-per-invocation counts, receipt/terminal attribution,
normal later processing, native journal reconciliation, nested TLS restoration,
cross-Engine thread separation, callback-thread health reads, counter saturation,
fixed logs without secret exception strings, direct non-Engine observe calls,
and close waiting for a deliberately blocked callback. These are deterministic
host tests, not mobile qualification or exhaustive concurrent lifecycle proof.
Sanitizer results and any runtime limitations are in the phase publication record.

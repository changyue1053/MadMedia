# M14a: stable callback query guard during close

Follow-up inspection found that synchronous query rejection still inspected the
callback worker unique_ptr, which close resets before waiting for the worker.
The guard now reads an immutable callback thread ID kept in Impl, so it does not
race that ownership reset or lose the callback-thread rejection during drain.

The existing deterministic close test gates on the actual callback pthread_join
entry before invoking both query methods; each must return State while callback
health retains its prior count. Native external calls racing destruction remain
unsupported. No API/layout, resource-limit or mobile-support claim is added.
Final sanitizer and CI evidence is recorded separately; no release.

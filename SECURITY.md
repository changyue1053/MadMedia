# Security / publication notes

This is a development baseline, not an audited release. Native engine retains upstream control protocol and filesystem/journal behavior. Host must use trusted service endpoints, account-private directories, and a documented authentication integration; current protocol does not expose a generic credentials provider. No endpoint or token is baked into the SDK.

Testkit's `mockctl-secret` is deliberately public, loopback-test-only, and must never be used for a real service. Testkit is excluded from installed packages.

Telemetry adapter exports only bounded enums. Native diagnostic logs are separate and may contain task IDs or error messages; host must disable them for sensitive deployments. Do not infer total log privacy from typed telemetry filtering.

Known pre-production audit topics: path/task-id validation, concurrent lifecycle safety, journal failure/atomicity, resource limits, image parser fuzz, real service auth, secure mobile background transfer and all platform bindings. This baseline must not be advertised as production-ready.

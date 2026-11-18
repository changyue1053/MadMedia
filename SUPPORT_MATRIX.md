# Support matrix

| Platform | Stage M01 |
|---|---|
| Linux x86_64 / GCC 14 | Native build and local tests; development C ABI v1, see TASK_RESULT and docs/C_ABI.md |
| iOS / simulator | Not built or device-tested; XCFramework/Swift binding absent |
| Android | Not built or device-tested; AAR/JNI/Kotlin binding absent |
| OHOS | Not built or device-tested; HAR/NAPI/ArkTS binding absent |
| Windows | Not supported by existing POSIX test/runtime paths |

Package requires host libcurl/SQLite3/Threads. It does not vendor or hide these symbols; multi-SDK dependency conflict qualification is still pending. Testkit is not part of installed package. No production media service, real weak-network matrix or mobile background transfer was verified.

`MADMEDIA_BUILD_C_ABI` defaults ON only for Linux. Other targets default OFF, and explicit enable is rejected until platform-specific entropy/lifecycle implementation is verified. Native C++ and its optional MadObs adapter do not require this C target.

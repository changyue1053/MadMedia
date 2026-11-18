# Support matrix

| Platform | Stage M01 |
|---|---|
| Linux x86_64 / GCC 14 | Native build and local tests, see TASK_RESULT |
| iOS / simulator | Not built or device-tested; XCFramework/Swift binding absent |
| Android | Not built or device-tested; AAR/JNI/Kotlin binding absent |
| OHOS | Not built or device-tested; HAR/NAPI/ArkTS binding absent |
| Windows | Not supported by existing POSIX test/runtime paths |

Package requires host libcurl/SQLite3/Threads. It does not vendor or hide these symbols; multi-SDK dependency conflict qualification is still pending. Testkit is not part of installed package. No production media service, real weak-network matrix or mobile background transfer was verified.

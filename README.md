# MadMedia

Independent C++17 media processing/transfer SDK. Development baseline, **not a production or mobile release**.

Stage M01 preserves the IMMedia engine (upload/download, journal, retry, streaming upload, image pipeline, cache) while adding a standalone CMake package and Mad public façade. Core has **no MadLink, MadDB, MadCrypto, MadInfer or MadObs runtime dependency**.

## Build

Install CMake >=3.16, a C++17 compiler, libcurl and SQLite3 development packages.

```sh
cmake -S . -B build -DMADMEDIA_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
cmake --install build --prefix "$PWD/stage"
cmake -S examples/standalone -B consumer -DCMAKE_PREFIX_PATH="$PWD/stage"
cmake --build consumer
./consumer/madmedia_consumer
```

Tests include unit and loopback mock-server cases (Python3, POSIX fork/socket required). They do not validate a production object store or three mobile platforms. `MADMEDIA_BUILD_TESTS=OFF` produces the library without test executables. Tools are opt-in (`MADMEDIA_BUILD_TOOLS=ON`).

```cpp
#include <madmedia/madmedia.h>
madmedia::Config config;
config.base_url = "https://your-media-control-service";
config.data_dir = "/app-private/account/media";
auto result = madmedia::start(config, [](const madmedia::ProgressEvent& event) {
  // callback thread; dispatch to UI yourself; do not destroy the engine here
});
```

Installed adapter consumers can use `find_package(MadMedia REQUIRED COMPONENTS Obs)` and link `MadMedia::obs`, with `MadMedia_MADOBS_INCLUDE_DIR` pointing to the contract include directory. Core-only `find_package(MadMedia REQUIRED)` does not discover or require MadObs.

The host owns service deployment, authentication integration and account-specific paths. The inherited control protocol is documented in `docs/INTEGRATION.md`; arbitrary upload services are not automatically supported. Native default logs can include diagnostic IDs/errors: disable via `immd::set_log_level(immd::LogLevel::Off)` when required. The typed observation adapter does NOT forward native log strings.

## Optional MadObs

The default observer is empty/Noop. Custom observers need only `madmedia/madmedia.h`. For MadObs, use `madmedia/obs_adapter.h` plus the separately supplied MadObs `observer.h` contract. Build adapter tests with:

```sh
cmake -S . -B build-obs -DMADMEDIA_WITH_MADOBS=ON -DMADOBS_INCLUDE_DIR=/path/to/MadObs/include
cmake --build build-obs -j4
ctest --test-dir build-obs --output-on-failure -R sdk
```

`madmedia_obs::make_observer(observer, sdk_id, counters)` exports only finite task-kind/state/error enums. No paths, task IDs, media IDs, sizes, MIME strings, signed URLs or error text. Host must register the chosen nonzero SDK ID/schema with its runtime; callback invocation is **not** proof of runtime admission or remote delivery. Observer must return promptly; it shares the engine callback thread, so arbitrary blocking still delays user callbacks. See `PUBLIC_API.md`.

## Scope and status

- C++ façade is a source-level 0.x API. A Linux-validated development C ABI v1 now ships as `MadMedia::ffi`; see `docs/C_ABI.md`. It is **not a stable 1.0 ABI or mobile support claim**. `immd` internals are retained to preserve existing behavior and data compatibility.
- iOS/Android/OHOS packages, target-specific C ABI entropy/lifecycle validation, upload-provider injection, background transfer bridges, production credentials and weak-network/device qualification remain future work.
- No IMSDK or message-delivery orchestration is included.
- Source provenance and publication constraints: `MIGRATION_SOURCE.toml`, `THIRD_PARTY_NOTICES.md`, `SECURITY.md`, `SUPPORT_MATRIX.md`, `TASK_RESULT.md`.
- Upstream README is historical, not current verification evidence: `docs/upstream/IMMedia-README.md`.

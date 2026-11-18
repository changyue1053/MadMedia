#pragma once
#include "core/engine/engine.h"
#include "core/pipeline/image/image_pipeline.h"
#include <functional>
#include <utility>

namespace madmedia {
using Config = immd::Config;
using Engine = immd::Engine;
using UploadRequest = immd::UploadRequest;
using DownloadRequest = immd::DownloadRequest;
using ProgressEvent = immd::ProgressEvent;
using ProgressCallback = immd::ProgressCallback;
using TaskKind = immd::TaskKind;
using TaskState = immd::TaskState;
using Error = immd::Error;
using Err = immd::Err;
using Priority = immd::Priority;
// Only finite dimensions. No task/media IDs, paths, messages, sizes or URLs.
struct Observation {
  TaskKind kind;
  TaskState state;
  Err error;
};
using ObserverPort = std::function<void(const Observation&)>;
// Observer runs on the engine callback thread, never its scheduler. It MUST
// return promptly and must not destroy the engine or throw. Exceptions are
// contained below; arbitrary blocking cannot be made safe by this contract.
// Caller-owned contexts must outlive engine destruction. Unregistration while
// running is unsupported. Empty port is Noop and brings no MadObs dependency.
inline ProgressCallback observe(ProgressCallback callback, ObserverPort observer = {}) {
  return [callback = std::move(callback), observer = std::move(observer)](const ProgressEvent& e) {
    if (observer) {
      try { observer(Observation{e.kind, e.state, e.error}); } catch (...) {}
    }
    if (callback) {
      try { callback(e); } catch (...) {} // do not terminate the callback worker
    }
  };
}
inline immd::Expected<std::unique_ptr<Engine>> start(
    Config config, ProgressCallback callback = {}, ObserverPort observer = {}) {
  return Engine::start(std::move(config), observe(std::move(callback), std::move(observer)));
}
inline const char* version() noexcept { return "0.1.0-dev"; }
} // namespace madmedia

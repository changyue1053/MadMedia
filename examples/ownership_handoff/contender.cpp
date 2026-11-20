#include <madmedia/madmedia.h>
#include <atomic>
#include <cstdio>

static madmedia::Config config_for(const char* directory) {
  madmedia::Config config;
  config.base_url = "ownership-fixture-not-a-url";  // not a request endpoint
  config.data_dir = directory;
  config.cache_capacity = 0;
  config.journal_durability = madmedia::JournalDurability::Full;
  return config;
}

static bool is_idle(madmedia::Engine& engine) {
  const auto row = engine.query_persisted("absent_fixture");
  const auto health = engine.callback_health();
  return !row.ok() && row.error().code == madmedia::Err::NotFound &&
      health.faulted_invocations == 0 && !health.callback_fault && !health.observer_fault &&
      !health.terminal_callback_fault && !health.receipt_callback_fault;
}

int main(int argc, char** argv) {
  if (argc != 3) return 1;
  std::atomic<bool> callback_seen{false};
  const auto callback = [&](const madmedia::ProgressEvent&) { callback_seen.store(true); };
  const auto blocked = madmedia::start(config_for(argv[1]), callback);
  if (blocked.ok() || blocked.error().code != madmedia::Err::Busy) return 2;
  auto separate = madmedia::start(config_for(argv[2]), callback);
  if (!separate.ok() || !is_idle(*separate.value())) return 3;
  if (std::putchar('B') != 'B' || std::fflush(stdout) != 0 || std::getchar() != 'R') return 4;
  auto reopened = madmedia::start(config_for(argv[1]), callback);
  if (!reopened.ok() || !is_idle(*reopened.value()) || !is_idle(*separate.value())) return 5;
  const auto second_b = madmedia::start(config_for(argv[2]), callback);
  if (second_b.ok() || second_b.error().code != madmedia::Err::Busy) return 6;
  reopened.value().reset();
  separate.value().reset();
  if (callback_seen.load()) return 7;
  return std::putchar('P') == 'P' && std::fflush(stdout) == 0 ? 0 : 8;
}

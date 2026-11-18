#include "core/compute/pool.h"

namespace immd {

ComputePool::ComputePool(int threads) {
  if (threads < 1) threads = 1;
  try {
  for (int i = 0; i < threads; ++i) {
    workers_.emplace_back([this] {
      while (true) {
        auto job = q_.pop(-1);
        if (!job.has_value()) {
          if (q_.drained()) return;
          continue;
        }
        job->fn(*job->flag);
        std::lock_guard<std::mutex> lk(mu_);
        flags_.erase(job->id);
      }
    });
  }
  } catch (...) {
    q_.close();
    for (auto& w : workers_) if (w.joinable()) w.join();
    throw;
  }
}

ComputePool::~ComputePool() {
  q_.close();
  for (auto& w : workers_) w.join();
}

uint64_t ComputePool::submit(Fn fn) {
  Job j;
  j.id = next_id_.fetch_add(1);
  j.fn = std::move(fn);
  j.flag = std::make_shared<std::atomic<bool>>(false);
  {
    std::lock_guard<std::mutex> lk(mu_);
    flags_[j.id] = j.flag;
  }
  uint64_t id = j.id;
  if (!q_.push(std::move(j))) {
    std::lock_guard<std::mutex> lk(mu_);
    flags_.erase(id);
    return 0;
  }
  return id;
}

void ComputePool::cancel(uint64_t id) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = flags_.find(id);
  if (it != flags_.end()) it->second->store(true);
}

}  // namespace immd


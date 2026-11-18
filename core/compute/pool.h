// 计算池（架构 §9）：hash / 快照拷贝 / 校验等 CPU+IO 密集任务。
// 全部任务协作式可取消——检查点粒度 64KB，用户撤销 10GB 任务不陪跑。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/util/queue.h"

namespace immd {

class ComputePool {
 public:
  using CancelFlag = std::shared_ptr<std::atomic<bool>>;
  using Fn = std::function<void(const std::atomic<bool>& canceled)>;

  explicit ComputePool(int threads);
  ~ComputePool();

  uint64_t submit(Fn fn);
  void cancel(uint64_t id);  // 协作式：置标志，任务在下一检查点退出

 private:
  struct Job {
    uint64_t id = 0;
    Fn fn;
    CancelFlag flag;
  };
  BoundedQueue<Job> q_{1024};
  std::vector<std::thread> workers_;
  std::mutex mu_;
  std::map<uint64_t, CancelFlag> flags_;
  std::atomic<uint64_t> next_id_{1};
};

}  // namespace immd


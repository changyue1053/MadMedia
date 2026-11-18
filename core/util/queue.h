// 有界 MPSC 队列：调度 Actor 的唯一输入口。
// 设计（架构 §9）：外部命令 try_push 快速失败（背压给调用方）；
// 内核事件 push 阻塞（事件量受在途预算约束，天然有界，不会死等）。
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace immd {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(size_t cap) : cap_(cap) {}

  // 阻塞投递；队列关闭返回 false。
  bool push(T v) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_not_full_.wait(lk, [&] { return closed_ || q_.size() < cap_; });
    if (closed_) return false;
    q_.push_back(std::move(v));
    cv_not_empty_.notify_one();
    return true;
  }

  // 非阻塞投递；满或关闭返回 false（调用方转 Err::Busy）。
  bool try_push(T v) {
    std::lock_guard<std::mutex> lk(mu_);
    if (closed_ || q_.size() >= cap_) return false;
    q_.push_back(std::move(v));
    cv_not_empty_.notify_one();
    return true;
  }

  // 取出一个元素；最多等到 timeout_ms（<0 表示无限等）。
  // 返回 nullopt 表示超时，或队列已关闭且排空（drained() 区分）。
  std::optional<T> pop(int64_t timeout_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    auto ready = [&] { return closed_ || !q_.empty(); };
    if (timeout_ms < 0) {
      cv_not_empty_.wait(lk, ready);
    } else {
      cv_not_empty_.wait_for(lk, std::chrono::milliseconds(timeout_ms), ready);
    }
    if (q_.empty()) return std::nullopt;
    T v = std::move(q_.front());
    q_.pop_front();
    cv_not_full_.notify_one();
    return v;
  }

  // 关闭：拒绝新投递；已入队元素仍可被 pop 排空（drain 语义，架构 §9 关闭排水）。
  void close() {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    cv_not_empty_.notify_all();
    cv_not_full_.notify_all();
  }

  bool drained() {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_ && q_.empty();
  }

  size_t size() {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }

 private:
  const size_t cap_;
  std::mutex mu_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_not_full_;
  std::deque<T> q_;
  bool closed_ = false;
};

}  // namespace immd


// 媒体缓存（架构 §18 端侧）：内容寻址（文件名 = sha256）+ LRU 容量淘汰。
// 内容寻址 → 缓存永不失效只有淘汰；淘汰回调是与 IMDB attachment 双向通知的接缝。
#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "core/util/error.h"

namespace immd {

class MediaCache {
 public:
  using EvictCb = std::function<void(const std::string& sha_hex)>;

  // 打开即扫描目录重建索引（mtime 作为初始 recency）
  static Expected<std::unique_ptr<MediaCache>> open(const std::string& dir, int64_t capacity_bytes);

  // 纳管文件（rename 优先，跨卷退化为拷贝）；超容量触发 LRU 淘汰
  Error put(const std::string& sha_hex, const std::string& src_path);
  // 命中返回缓存路径并刷新 recency；未命中 Err::NotFound
  Expected<std::string> lookup(const std::string& sha_hex);
  void remove(const std::string& sha_hex);
  int64_t total_bytes();
  void set_evict_cb(EvictCb cb);

 private:
  MediaCache(std::string dir, int64_t cap) : dir_(std::move(dir)), cap_(cap) {}
  std::string path_of(const std::string& sha_hex) const { return dir_ + "/" + sha_hex; }
  void evict_locked();

  std::string dir_;
  int64_t cap_;
  std::mutex mu_;
  // LRU：list 头部最新；map 存迭代器
  std::list<std::pair<std::string, int64_t>> lru_;  // (sha, size)
  std::map<std::string, std::list<std::pair<std::string, int64_t>>::iterator> index_;
  int64_t total_ = 0;
  EvictCb evict_cb_;
};

}  // namespace immd


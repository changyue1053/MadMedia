#include "core/cache/cache.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <vector>

#include "core/util/fs.h"

namespace immd {

Expected<std::unique_ptr<MediaCache>> MediaCache::open(const std::string& dir, int64_t capacity_bytes) {
  if (Error e = mkdirs(dir)) return e;
  auto c = std::unique_ptr<MediaCache>(new MediaCache(dir, capacity_bytes));
  // 重建索引：mtime 升序 → 老文件排在 LRU 尾部（先被淘汰）
  std::vector<std::pair<int64_t, std::pair<std::string, int64_t>>> found;  // (mtime, (sha, size))
  DIR* d = opendir(dir.c_str());
  if (!d) return make_err(Err::Io, "opendir failed: " + dir);
  while (dirent* ent = readdir(d)) {
    std::string name = ent->d_name;
    if (name == "." || name == "..") continue;
    struct stat st{};
    if (stat((dir + "/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
    found.push_back({int64_t(st.st_mtime), {name, int64_t(st.st_size)}});
  }
  closedir(d);
  std::sort(found.begin(), found.end());  // 老 → 新
  for (auto& f : found) {                 // push_front：新的最终在头部
    c->lru_.push_front(f.second);
    c->index_[f.second.first] = c->lru_.begin();
    c->total_ += f.second.second;
  }
  return c;
}

Error MediaCache::put(const std::string& sha_hex, const std::string& src_path) {
  auto sz = file_size(src_path);
  if (!sz.ok()) return sz.error();
  std::string dst = path_of(sha_hex);
  if (rename(src_path.c_str(), dst.c_str()) != 0) {  // 跨卷退化为拷贝
    std::ifstream in(src_path, std::ios::binary);
    std::ofstream out(dst, std::ios::binary);
    if (!in || !out) return make_err(Err::Io, "cache copy failed");
    out << in.rdbuf();
    if (!out.good()) return make_err(Err::Io, "cache copy write failed");
  }
  std::lock_guard<std::mutex> lk(mu_);
  auto it = index_.find(sha_hex);
  if (it != index_.end()) {  // 已存在（内容寻址幂等）：刷新 recency
    total_ -= it->second->second;
    lru_.erase(it->second);
    index_.erase(it);
  }
  lru_.push_front({sha_hex, sz.value()});
  index_[sha_hex] = lru_.begin();
  total_ += sz.value();
  evict_locked();
  return {};
}

Expected<std::string> MediaCache::lookup(const std::string& sha_hex) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = index_.find(sha_hex);
  if (it == index_.end()) return make_err(Err::NotFound, "cache miss");
  lru_.splice(lru_.begin(), lru_, it->second);  // touch
  index_[sha_hex] = lru_.begin();
  return path_of(sha_hex);
}

void MediaCache::remove(const std::string& sha_hex) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = index_.find(sha_hex);
  if (it == index_.end()) return;
  total_ -= it->second->second;
  lru_.erase(it->second);
  index_.erase(it);
  remove_file(path_of(sha_hex));
}

int64_t MediaCache::total_bytes() {
  std::lock_guard<std::mutex> lk(mu_);
  return total_;
}

void MediaCache::set_evict_cb(EvictCb cb) {
  std::lock_guard<std::mutex> lk(mu_);
  evict_cb_ = std::move(cb);
}

void MediaCache::evict_locked() {
  while (total_ > cap_ && !lru_.empty()) {
    auto& victim = lru_.back();
    total_ -= victim.second;
    std::string sha = victim.first;
    index_.erase(sha);
    lru_.pop_back();
    remove_file(path_of(sha));
    if (evict_cb_) evict_cb_(sha);  // IMDB attachment 元数据标记的接缝
  }
}

}  // namespace immd


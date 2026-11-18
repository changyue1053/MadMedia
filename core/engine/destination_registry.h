#pragma once
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include "core/util/error.h"
namespace immd {
// Cooperative, case-sensitive filesystem admission. Host must not mutate path
// topology while Engine exists, or supply external bind aliases of private
// descendants (their ancestry is hidden). This is NOT a security sandbox.
// Accepted reservations last until destruction.
class DestinationRegistry {
 struct Entry { std::string path; std::array<std::string,3> keys, inodes; };
 std::mutex mu_;
 std::map<std::string,Entry> entries_;
 std::string root_;
 std::set<std::string> private_dirs_, private_keys_, private_inodes_;
 static std::string inode(const struct stat& s) { return std::to_string(s.st_dev)+":"+std::to_string(s.st_ino); }
 static std::string key(const struct stat& s,const std::string& name) { return inode(s)+":"+name; }
 static Error invalid(){return make_err(Err::State,"download destination rejected");}
 static bool beneath(const std::string& p,const std::string& dir){return p==dir||(p.size()>dir.size()&&p.compare(0,dir.size(),dir)==0&&p[dir.size()]=='/');}
 Expected<Entry> inspect(const std::string& path) const {
  namespace fs=std::filesystem;
  if(path.empty()||path.find('\0')!=std::string::npos)return invalid();
  fs::path input(path);auto leaf=input.filename().string();
  if(leaf.empty()||leaf=="."||leaf=="..")return invalid();
  std::error_code ec;auto parent=fs::canonical(input.has_parent_path()?input.parent_path():fs::path("."),ec);
  if(ec)return invalid();
  struct stat ps{};if(::stat(parent.c_str(),&ps)||!S_ISDIR(ps.st_mode))return invalid();
  for(auto walk=parent;;walk=walk.parent_path()){
   struct stat ds{};if(::stat(walk.c_str(),&ds))return invalid();
   if(private_dirs_.count(inode(ds)))return invalid();
   if(walk==walk.root_path())break;
  }
  Entry e;e.path=(parent/leaf).string();
  const std::array<std::string,3> names{leaf,leaf+".part",leaf+".cachetmp"};
  for(size_t i=0;i<3;++i){
   auto candidate=(parent/names[i]).string();
   if(beneath(candidate,root_+"/staging")||beneath(candidate,root_+"/cache"))return invalid();
   e.keys[i]=key(ps,names[i]);if(private_keys_.count(e.keys[i]))return invalid();
   struct stat ls{};
   if(::lstat(candidate.c_str(),&ls)==0){
    if(!S_ISREG(ls.st_mode)||ls.st_nlink!=1)return invalid();
    e.inodes[i]=inode(ls);if(private_inodes_.count(e.inodes[i]))return invalid();
   }else if(errno!=ENOENT)return invalid();
  }
  return e;
 }
 public:
 static constexpr size_t capacity=4096;
 Error initialize(const std::string& data_dir){
  std::error_code ec;root_=std::filesystem::canonical(data_dir,ec).string();if(ec)return invalid();
  struct stat root{};if(::stat(root_.c_str(),&root))return invalid();
  for(const auto* name:{"journal.db","journal.db-wal","journal.db-shm","journal.db-journal",".madmedia.lock"}){
   private_keys_.insert(key(root,name));struct stat s{};
   if(::lstat((root_+"/"+name).c_str(),&s)==0)private_inodes_.insert(inode(s));
   else if(errno!=ENOENT)return invalid();
  }
  for(const auto* name:{"staging","cache"}){struct stat s{};
   if(::stat((root_+"/"+name).c_str(),&s)==0)private_dirs_.insert(inode(s));
   else if(errno!=ENOENT)return invalid();
  }
  return {};
 }
 Expected<std::string> reserve(const std::string& id,const std::string& path){
  auto candidate=inspect(path);if(!candidate.ok())return candidate.error();
  auto& e=candidate.value();Expected<std::string> result(e.path);
  std::lock_guard<std::mutex> lock(mu_);
  if(entries_.size()>=capacity)return make_err(Err::Busy,"download destination capacity reached");
  for(const auto& item:entries_)for(size_t i=0;i<3;++i)for(size_t j=0;j<3;++j)
   if(e.keys[i]==item.second.keys[j]||(!e.inodes[i].empty()&&e.inodes[i]==item.second.inodes[j]))return invalid();
  if(!entries_.emplace(id,std::move(e)).second)return invalid();
  return result;
 }
 void rollback(const std::string& id){std::lock_guard<std::mutex> lock(mu_);entries_.erase(id);}
 Expected<std::string> find(const std::string& id){std::lock_guard<std::mutex> lock(mu_);auto it=entries_.find(id);if(it==entries_.end())return invalid();return it->second.path;}
};
} // namespace immd

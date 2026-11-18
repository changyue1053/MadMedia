#pragma once
#include <memory>
#include <string>
#include "core/util/error.h"
namespace immd {
// Cooperative local-filesystem ownership, held until all Engine storage closes.
// Never unlink the lock file: inode replacement would invalidate mutual exclusion.
class DirectoryLock {
 public:
 static Expected<std::unique_ptr<DirectoryLock>> acquire(const std::string& directory);
 ~DirectoryLock();
 DirectoryLock(const DirectoryLock&)=delete;
 DirectoryLock& operator=(const DirectoryLock&)=delete;
 private:
 explicit DirectoryLock(int fd):fd_(fd){}
 int fd_=-1;
};
}

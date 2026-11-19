#pragma once
#include "lease.cpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace apple_preview {
inline bool exercise(const std::string& base) {
  const std::string directory = base + "/private";
  const std::string alias = base + "/alias";
  const std::string lock = directory + "/" + apple_preview::lock_name;
  const std::string file = base + "/file";
  if (mkdir(directory.c_str(), 0700) != 0) return false;
  Lease one, two;
  if (one.acquire(directory.c_str()) != Result::acquired ||
      one.acquire(directory.c_str()) != Result::refused ||
      two.acquire(directory.c_str()) != Result::busy) return false;
  one.release();
  if (two.acquire(directory.c_str()) != Result::acquired) return false;
  two.release();
  two.release();
  if (one.acquire(directory.c_str()) != Result::acquired) return false;
  one.release();

  if (symlink(directory.c_str(), alias.c_str()) != 0 ||
      two.acquire(alias.c_str()) != Result::refused ||
      unlink(alias.c_str()) != 0 ||
      chmod(directory.c_str(), 0750) != 0 ||
      two.acquire(directory.c_str()) != Result::refused ||
      chmod(directory.c_str(), 0700) != 0 ||
      two.acquire(nullptr) != Result::refused ||
      two.acquire("relative") != Result::refused) return false;

  apple_preview::Fd ordinary(open(file.c_str(), O_RDONLY | O_CREAT | O_EXCL, 0600));
  if (ordinary.get() < 0 || two.acquire(file.c_str()) != Result::refused ||
      unlink(lock.c_str()) != 0 ||
      symlink(file.c_str(), lock.c_str()) != 0 ||
      two.acquire(directory.c_str()) != Result::refused ||
      unlink(lock.c_str()) != 0 ||
      link(file.c_str(), lock.c_str()) != 0 ||
      two.acquire(directory.c_str()) != Result::refused ||
      unlink(lock.c_str()) != 0 ||
      mkfifo(lock.c_str(), 0600) != 0 ||
      two.acquire(directory.c_str()) != Result::refused ||
      unlink(lock.c_str()) != 0) return false;
  return two.acquire(directory.c_str()) == Result::acquired;
}
}

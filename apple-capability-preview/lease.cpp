// Standalone prototype only. Not compiled into or used by MadMedia.
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace apple_preview {
enum class Result { acquired, busy, refused };
constexpr char lock_name[] = ".capability.lock";

class Fd {
 public:
  explicit Fd(int value = -1) noexcept : value_(value) {}
  ~Fd() { if (value_ >= 0) (void)close(value_); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
  int get() const noexcept { return value_; }
  void swap(Fd& other) noexcept { std::swap(value_, other.value_); }
 private:
  int value_;
};

class Lease {
 public:
  void release() noexcept { Fd old; old.swap(fd_); }
  Result acquire(const char* directory) noexcept {
    if (fd_.get() != -1 || !directory || directory[0] != '/') return Result::refused;
    Fd dir(open(directory, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY));
    struct stat st{};
    if (dir.get() < 0 || fstat(dir.get(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 07777) != 0700) return Result::refused;
    // No truncation; FIFO/open cannot block; reject anything but a private regular inode.
    Fd lock(openat(dir.get(), lock_name, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600));
    if (lock.get() < 0 || fstat(lock.get(), &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || st.st_nlink != 1 || (st.st_mode & 07777) != 0600) return Result::refused;
    int rc;
    do { rc = flock(lock.get(), LOCK_EX | LOCK_NB); } while (rc < 0 && errno == EINTR);
    if (rc != 0) return errno == EWOULDBLOCK || errno == EAGAIN ? Result::busy : Result::refused;
    lock.swap(fd_);
    return Result::acquired;
  }
 private:
  Fd fd_;
};
} // namespace apple_preview

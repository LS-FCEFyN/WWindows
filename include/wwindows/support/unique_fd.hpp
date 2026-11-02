#pragma once

#include <unistd.h>
#include <utility>

namespace ww {

// Owns a POSIX file descriptor; closes on destruction.
// Move-only, same semantics as unique_ptr.
class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) noexcept : fd_{fd} {}

  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&o) noexcept : fd_{std::exchange(o.fd_, -1)} {}

  UniqueFd &operator=(UniqueFd &&o) noexcept {
    if (this != &o) {
      reset();
      fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  // Release ownership without closing
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  void reset(int new_fd = -1) noexcept {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = new_fd;
  }

private:
  int fd_ = -1;
};

} // namespace ww
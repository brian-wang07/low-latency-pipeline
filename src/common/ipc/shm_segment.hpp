#pragma once

#include "shm.hpp"
#include <cstddef>
#include <new>

class ShmSegment {
public:
  ShmSegment() = default;
  ~ShmSegment() noexcept;

  ShmSegment(const ShmSegment &) = delete;
  ShmSegment &operator=(const ShmSegment &) = delete;

  ShmSegment(ShmSegment &&other) noexcept;
  ShmSegment &operator=(ShmSegment &&other) noexcept;

  bool create(const char *label = ipc::SHM_NAME,
              std::size_t size = ipc::SHM_SIZE);

  bool attach(int fd, std::size_t size = ipc::SHM_SIZE);

  int fd() const noexcept { return fd_; }
  void *get_address() const noexcept { return addr_; }
  std::size_t get_size() const noexcept { return size_; }
  bool is_valid() const noexcept { return is_valid_; }

  template <typename T> T *as() const noexcept {
    return std::launder(reinterpret_cast<T *>(addr_));
  }

private:
  void reset_() noexcept;

  int fd_{-1};
  void *addr_{nullptr};
  std::size_t size_{0};
  bool owner_{false};
  bool is_valid_{false};
};

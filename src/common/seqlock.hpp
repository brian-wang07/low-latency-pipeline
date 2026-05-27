#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace common {
template <typename T> struct alignas(64) Seqlock {
  static_assert(std::is_trivially_copyable_v<T>);

  std::atomic<uint64_t> seq{0};
  T data;

  void store(const T &in) noexcept {
    uint64_t s = seq.load(std::memory_order_relaxed);
    seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(&data, &in, sizeof(T));
    seq.store(s + 2, std::memory_order_release);
  }

  bool try_load(T &out) noexcept {
    uint64_t pre = seq.load(std::memory_order_acquire);
    if (pre & 1)
      return false;
    std::memcpy(&out, &data, sizeof(T));
    std::atomic_thread_fence(std::memory_order_acquire);
    return pre == seq.load(std::memory_order_relaxed);
  }
};

} // namespace common

#pragma once

#include <atomic>
#include <cstdint>

namespace common {

template <typename T, uint32_t capacity> struct alignas(64) SpscRing {
  // capacity must be power of 2
  static_assert((capacity & (capacity - 1)) == 0);

  static constexpr uint32_t MASK = capacity - 1;

  alignas(64) std::atomic<uint32_t> head{0};
  alignas(64) std::atomic<uint32_t> tail{0};

  alignas(64) T slots[capacity];

  bool try_push(const T &item) noexcept {

    uint32_t write_idx = tail.load(std::memory_order_relaxed);
    uint32_t read_idx = head.load(std::memory_order_acquire);

    if (write_idx - read_idx == capacity)
      return false;

    slots[write_idx & MASK] = item;
    tail.store(write_idx + 1, std::memory_order_release);

    return true;
  };

  template <typename... Args> bool try_emplace(Args &&...args) noexcept {
    // basically always prefer this, Event type has no heap allocated fields

    uint32_t write_idx = tail.load(std::memory_order_relaxed);
    uint32_t read_idx = head.load(std::memory_order_acquire);

    if (write_idx - read_idx == capacity)
      return false;

    new (&slots[write_idx & MASK]) T(std::forward<Args>(args)...);
    tail.store(write_idx + 1, std::memory_order_release);

    return true;
  }

  bool try_pop(T &out) noexcept {

    uint32_t read_idx = head.load(std::memory_order_relaxed);
    uint32_t write_idx = tail.load(std::memory_order_acquire);

    if (write_idx == read_idx)
      return false;

    out = slots[read_idx & MASK];
    head.store(read_idx + 1, std::memory_order_release);

    return true;
  }

  uint32_t size() const noexcept {

  };
};

} // namespace common
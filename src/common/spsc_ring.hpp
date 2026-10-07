#pragma once

#include <atomic>
#include <cstdint>
#include <new>
#include <utility>

namespace common {

template <typename T, uint32_t capacity> struct alignas(64) SpscRing {
  // capacity must be power of 2
  static_assert((capacity & (capacity - 1)) == 0);

  static constexpr uint32_t MASK = capacity - 1;

  alignas(64) std::atomic<uint32_t> head{0};
  alignas(64) std::atomic<uint32_t> tail{0};

  alignas(64) T slots[capacity];

  // Two-phase produce: fill the claimed slot in place, then publish(). Lets the
  // producer make its last store (e.g. tsc_in) right before the release, and skips
  // the copy of a staged T. Repeated claims without a publish return the same slot.
  T *try_claim() noexcept {
    uint32_t write_idx = tail.load(std::memory_order_relaxed);
    uint32_t read_idx = head.load(std::memory_order_acquire);

    if (write_idx - read_idx == capacity)
      return nullptr;

    return &slots[write_idx & MASK];
  }

  void publish() noexcept {
    tail.store(tail.load(std::memory_order_relaxed) + 1,
               std::memory_order_release);
  }

  bool try_push(const T &item) noexcept {
    T *slot = try_claim();
    if (!slot)
      return false;
    *slot = item;
    publish();
    return true;
  }

  template <typename... Args> bool try_emplace(Args &&...args) noexcept {
    T *slot = try_claim();
    if (!slot)
      return false;
    new (slot) T(std::forward<Args>(args)...);
    publish();
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
    // May be called by a third party (e.g. the dashboard), so head and tail are
    // sampled at different instants.
    uint32_t h = head.load(std::memory_order_acquire);
    uint32_t t = tail.load(std::memory_order_acquire);
    return t - h;
  };
};

} // namespace common
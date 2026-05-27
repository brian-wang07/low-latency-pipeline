#pragma once

#include "common/platform/cpu_pin.hpp"
#include "core/snapshot/book_snapshot.hpp"
#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <thread>

namespace core {

template <typename C, std::size_t depth>
concept SnapshotCallback = requires(C cb, const BookSnapshot<depth> &s) {
  { cb(s) } -> std::same_as<void>;
};

template <std::size_t depth, SnapshotCallback<depth> Callback>
void run_snapshotter(BookSeqlock<depth> &src, int pin_core,
                     std::chrono::microseconds period,
                     const std::atomic<bool> &shutdown, Callback cb) {
  if (!pin_to_core(pin_core))
    std::perror("snapshotter pin_to_core");

  BookSnapshot<depth> local;
  auto next = std::chrono::steady_clock::now() + period;

  while (!shutdown.load(std::memory_order_acquire)) {
    std::this_thread::sleep_until(next);
    next += period;

    // try up to 64 times
    int tries = 0;
    while (tries < 64 && !src.try_load(local))
      ++tries;
    if (tries >= 64)
      continue;
    if (local.event_seq == 0)
      continue;
    // publish
    // cb(local);
  }
}

} // namespace core

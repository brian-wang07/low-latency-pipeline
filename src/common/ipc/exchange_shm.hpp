#pragma once

#include "common/event.hpp"
#include "common/spsc_ring.hpp"
#include <atomic>
#include <cstdint>

namespace exchange {

// matching engine data feed
// producers: itch parser, strategy process
// consumer: matching engine
inline constexpr std::size_t MAX_EXCHANGE_PRODUCERS = 2;
inline constexpr std::uint32_t EXCHANGE_RING_CAPACITY = 4096;

using ExchangeRing = common::SpscRing<common::Event, EXCHANGE_RING_CAPACITY>;

struct alignas(64) ExchangeInputArray {
  ExchangeRing ExchangeRings[MAX_EXCHANGE_PRODUCERS];
};

} // namespace exchange
#pragma once

#include <cstdint>

#include "common/event.hpp"
#include "common/spsc_ring.hpp"

namespace core {

// routes to dashboard, main stuff
// producer: exchange
// consumer: core
inline constexpr std::uint32_t CORE_RING_CAPACITY = 8192;
using CoreRing = common::SpscRing<common::Event, CORE_RING_CAPACITY>;
} // namespace core

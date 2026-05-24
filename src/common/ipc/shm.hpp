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

namespace core {

// routes to dashboard, main stuff
// producer: exchange
// consumer: core
inline constexpr std::uint32_t CORE_RING_CAPACITY = 8192;
using CoreRing = common::SpscRing<common::Event, CORE_RING_CAPACITY>;
} // namespace core

namespace ipc {
inline constexpr const char *SHM_NAME = "pipeline_shm";
inline constexpr size_t SHM_SIZE = 16 * 1024 * 1024;
inline constexpr uint64_t MAGIC = 0xDEADBEEF;
inline constexpr uint32_t VERSION = 1;
static_assert((SHM_SIZE & (SHM_SIZE - 1)) == 0);

struct alignas(64) ShmHeader {
  std::atomic<uint64_t> magic;
  uint32_t version;
  uint32_t _pad;
};

struct alignas(64) PipelineShm {
  ShmHeader header;
  core::CoreRing exchange_to_core;
};

} // namespace ipc
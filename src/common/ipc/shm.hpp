#pragma once

#include "common/event.hpp"
#include "common/ipc/control.hpp"
#include "common/ipc/dashboard_snapshot.hpp"
#include "common/ipc/instrument.hpp"
#include "common/ipc/market_update.hpp"
#include "common/ipc/order_msgs.hpp"
#include "common/ipc/stats.hpp"
#include "common/seqlock.hpp"
#include "common/spsc_ring.hpp"
#include <atomic>
#include <cstdint>

namespace core {

// routes to dashboard, main stuff
// producer: exchange
// consumer: core
inline constexpr std::uint32_t CORE_RING_CAPACITY = 8192;
using CoreRing = common::SpscRing<common::Event, CORE_RING_CAPACITY>;
} // namespace core

namespace exec {

// producer: core_main (ITCH) or the gateway MD thread (live), consumer: exec
inline constexpr std::size_t EXEC_DEPTH = 10;
inline constexpr std::uint32_t FEED_RING_CAPACITY = 8192;
using FeedRing = common::SpscRing<MarketUpdate<EXEC_DEPTH>, FEED_RING_CAPACITY>;
} // namespace exec

namespace dashboard {

// book snapshot frames for the GUI
// producer: core snapshotter thread, consumer: dashboard process
// DASH_DEPTH is part of the wire contract: producer and consumer must agree.
inline constexpr std::size_t DASH_DEPTH = 15;
// small, since we publish events every 30hz and read 60hz
inline constexpr std::uint32_t DASH_RING_CAPACITY = 64;
using DashboardRing =
    common::SpscRing<Snapshot<DASH_DEPTH>, DASH_RING_CAPACITY>;
} // namespace dashboard

namespace ipc {
inline constexpr const char *SHM_NAME = "pipeline_shm";
inline constexpr size_t SHM_SIZE = 16 * 1024 * 1024;
inline constexpr uint64_t MAGIC = 0xDEADBEEF;
inline constexpr uint32_t VERSION = 3;
static_assert((SHM_SIZE & (SHM_SIZE - 1)) == 0);

// Workers check version and layout_hash (ipc::LAYOUT_HASH in layout.hpp) and
// refuse to run against a segment built from a different layout.
struct alignas(64) ShmHeader {
  std::atomic<uint64_t> magic;
  uint32_t version;
  uint32_t layout_hash;
};

struct alignas(64) PipelineShm {
  ShmHeader header;
  Control control;
  ref::InstrumentTable instruments;
  core::CoreRing exchange_to_core;
  exec::FeedRing feed_to_exec;
  oe::OrderRing exec_to_gateway;
  oe::ExecRing gateway_to_exec;
  dashboard::DashboardRing core_to_dashboard;
  common::Seqlock<stats::FeedStats> feed_stats;
  common::Seqlock<stats::ExecStats> exec_stats;
  common::Seqlock<stats::GatewayStats> gateway_stats;
  common::Seqlock<stats::PositionStats> positions;
  common::Seqlock<stats::OpenOrders> open_orders;
};

static_assert(sizeof(PipelineShm) <= SHM_SIZE);

} // namespace ipc

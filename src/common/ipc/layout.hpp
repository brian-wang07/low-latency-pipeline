#pragma once

#include "common/ipc/shm.hpp"
#include <cstddef>
#include <cstdint>

namespace ipc {

namespace detail {
constexpr uint32_t fnv1a(uint32_t h, uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    h ^= static_cast<uint32_t>((v >> (8 * i)) & 0xff);
    h *= 16777619u;
  }
  return h;
}
} // namespace detail

// FNV-1a over the size, alignment and key offsets of every cross-process type.
// Any layout change moves it, so workers built from different headers than the
// manager refuse to attach. Add new wire types and fields here.
constexpr uint32_t compute_layout_hash() noexcept {
  using MU = exec::MarketUpdate<exec::EXEC_DEPTH>;
  using DS = dashboard::Snapshot<dashboard::DASH_DEPTH>;
  const uint64_t parts[] = {
      sizeof(PipelineShm),
      offsetof(PipelineShm, control),
      offsetof(PipelineShm, instruments),
      offsetof(PipelineShm, exchange_to_core),
      offsetof(PipelineShm, feed_to_exec),
      offsetof(PipelineShm, exec_to_gateway),
      offsetof(PipelineShm, gateway_to_exec),
      offsetof(PipelineShm, core_to_dashboard),
      offsetof(PipelineShm, feed_stats),
      offsetof(PipelineShm, exec_stats),
      offsetof(PipelineShm, gateway_stats),
      offsetof(PipelineShm, positions),
      offsetof(PipelineShm, open_orders),

      sizeof(common::Event), alignof(common::Event),
      offsetof(common::Event, tsc_in),

      sizeof(MU), alignof(MU), exec::EXEC_DEPTH,
      offsetof(MU, best_bid), offsetof(MU, trade_qty), offsetof(MU, instrument),
      offsetof(MU, flags), offsetof(MU, nb), offsetof(MU, bids),
      offsetof(MU, asks), sizeof(exec::Level),

      sizeof(oe::OrderRequest), offsetof(oe::OrderRequest, price),
      offsetof(oe::OrderRequest, instrument), offsetof(oe::OrderRequest, tif),
      sizeof(oe::ExecReport), offsetof(oe::ExecReport, last_px),
      offsetof(oe::ExecReport, instrument),
      offsetof(oe::ExecReport, venue_order_id),

      sizeof(ref::Instrument), offsetof(ref::Instrument, symbol),
      offsetof(ref::Instrument, tick_exp), offsetof(ref::Instrument, status),
      sizeof(ref::InstrumentTable),

      sizeof(Control), sizeof(DS), dashboard::DASH_DEPTH,

      sizeof(stats::ExecStats), sizeof(stats::FeedStats),
      sizeof(stats::GatewayStats), sizeof(stats::PositionStats),
      sizeof(stats::OpenOrders),
  };
  uint32_t h = 2166136261u;
  for (uint64_t p : parts)
    h = detail::fnv1a(h, p);
  return h;
}

inline constexpr uint32_t LAYOUT_HASH = compute_layout_hash();

} // namespace ipc

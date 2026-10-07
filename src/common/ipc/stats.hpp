#pragma once

#include "common/event.hpp"
#include "common/histogram.hpp"
#include "common/ipc/order_msgs.hpp"
#include <cstdint>
#include <type_traits>

// Seqlock payloads, each written by its owning thread and read by the dashboard
// and manager.
namespace stats {

struct ExecStats {
  common::Histogram feed_transit;  // pop - tsc_in
  common::Histogram tick;          // on_tick duration
  common::Histogram tick_to_order; // order push - tsc_in
  common::Histogram exec_transit;
  uint64_t frames, frame_gaps, orders_new, orders_cancel, risk_rejects,
      ring_full_rejects, iterations, heartbeat_tsc, exec_generation;
  uint32_t strategy_id;
  char strategy_name[32];
};

enum FeedMode : uint8_t { FEED_NONE = 0, FEED_ITCH = 1, FEED_VENUE = 2 };

// Written by the feed producer: core_main or the gateway MD thread.
struct FeedStats {
  uint64_t frames_pushed, frames_dropped, events_in, book_resets, heartbeat_tsc;
  uint8_t mode; // FeedMode
};

struct GatewayStats {
  common::Histogram parse, order_encode_send, ack_rtt, fill_rtt, cancel_rtt;
  uint64_t md_msgs, md_bytes, oe_msgs_in, oe_msgs_out, seq_gaps, resyncs,
      reconnects, last_md_tsc, last_oe_tsc, orders_sent, acks, fills, cancels,
      rejects, cancel_rejects, rate_budget_remaining, heartbeat_tsc;
  uint8_t md_state, oe_state;
  char venue[8];
};

struct Position {
  int64_t qty_lots, avg_px_ticks, realized_pnl, unrealized_pnl, fees, bought,
      sold, peak_equity, max_drawdown;
  uint32_t open_orders, fills, orders, cancels, rejects;
  uint16_t instrument;
};

struct PositionStats {
  Position rows[16];
  uint32_t count;
};

struct OpenOrder {
  uint64_t cl_ord_id, tsc_sent;
  int64_t px, qty, leaves;
  uint16_t instrument;
  common::Side side;
  oe::OrdStatus st;
};

struct OpenOrders {
  OpenOrder rows[64];
  uint32_t count;
};

static_assert(std::is_trivially_copyable_v<ExecStats>);
static_assert(std::is_trivially_copyable_v<FeedStats>);
static_assert(std::is_trivially_copyable_v<GatewayStats>);
static_assert(std::is_trivially_copyable_v<PositionStats>);
static_assert(std::is_trivially_copyable_v<OpenOrders>);

} // namespace stats

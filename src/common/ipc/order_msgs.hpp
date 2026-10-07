#pragma once

#include "common/event.hpp"
#include "common/spsc_ring.hpp"
#include <cstdint>
#include <type_traits>

// Order entry contracts between exec and a venue gateway. Prices and quantities
// are int64 ticks and lots of the instrument (see ref::InstrumentTable).
namespace oe {

enum class ReqType : uint8_t { New = 1, Cancel = 2, Replace = 3, CancelAll = 4 };
enum class OrdType : uint8_t { Limit = 1, Market = 2 };
// PostOnly maps to Coinbase ExecInst=A and Kraken post_only.
enum class Tif : uint8_t { GTC = 1, IOC = 2, FOK = 3, PostOnly = 4 };

struct alignas(64) OrderRequest {
  uint64_t cl_ord_id;      // (exec_epoch << 40) | counter, unique across exec restarts
  uint64_t orig_cl_ord_id; // Cancel/Replace target
  uint64_t tsc_decision;   // read_tsc() at submit
  uint64_t strategy_tag;   // opaque, echoed in ExecReport
  int64_t price;
  int64_t qty;
  uint16_t instrument;
  ReqType type;
  common::Side side;
  OrdType ord_type;
  Tif tif;
  uint8_t flags;
  uint8_t _pad;
};
static_assert(sizeof(OrderRequest) == 64);
static_assert(std::is_trivially_copyable_v<OrderRequest>);

enum class ExecType : uint8_t {
  Ack = 1,
  Reject = 2,
  PartialFill = 3,
  Fill = 4,
  Canceled = 5,
  CancelReject = 6,
  Replaced = 7,
  Expired = 8,
  Restated = 9,
};
enum class OrdStatus : uint8_t {
  PendingNew = 0,
  New,
  PartiallyFilled,
  Filled,
  Canceled,
  Rejected,
  Expired,
};

struct alignas(64) ExecReport {
  uint64_t cl_ord_id, orig_cl_ord_id, strategy_tag;
  uint64_t tsc_sent; // from the gateway's order table, 0 for unsolicited
  uint64_t tsc_recv; // gateway read_tsc() on receipt
  uint64_t venue_ts_ns;
  int64_t last_px, last_qty, cum_qty, leaves_qty;
  int64_t fee; // 1e-8 units of fee_ccy
  uint16_t instrument;
  ExecType exec_type;
  OrdStatus ord_status;
  uint8_t liquidity; // 0 unknown, 1 maker, 2 taker
  uint8_t reject_reason;
  uint8_t fee_ccy; // 0 quote, 1 base
  uint8_t _pad;
  char venue_order_id[32]; // Coinbase UUID text or Kraken order_id
};
static_assert(sizeof(ExecReport) == 128);
static_assert(std::is_trivially_copyable_v<ExecReport>);

// producer: exec, consumer: gateway OE thread
inline constexpr uint32_t ORDER_RING_CAPACITY = 4096;
using OrderRing = common::SpscRing<OrderRequest, ORDER_RING_CAPACITY>;
// producer: gateway OE thread, consumer: exec
inline constexpr uint32_t EXEC_RING_CAPACITY = 4096;
using ExecRing = common::SpscRing<ExecReport, EXEC_RING_CAPACITY>;

} // namespace oe

// internal event type
#pragma once

#include <cstdint>

namespace common {

enum class Side : int8_t { Sell = -1, Buy = 1 };

struct alignas(64) Event {
  char message_type;
  uint16_t stock_locate;
  uint16_t tracking_number;
  uint64_t timestamp;
  uint64_t order_ref_number;
  Side side;
  uint32_t shares;
  char stock[8];
  uint32_t price;
  uint64_t match_number{0};         // for execute, trade, broken trade
  uint64_t new_order_ref_number{0}; // for replace
  uint64_t tsc_in;
  char _pad[2];
};

} // namespace common

#pragma once

#include "common/seqlock.hpp"
#include "core/core.hpp"
#include <cstdint>
#include <cstring>

namespace core {

template <std::size_t depth = 10> struct alignas(64) BookSnapshot {
  uint64_t event_seq;
  char stock_id[8];
  uint32_t best_bid;
  uint32_t best_ask;
  int nb;
  int na;
  equity::OrderBook::Level bids[depth];
  equity::OrderBook::Level asks[depth];
};

template <std::size_t depth>
using BookSeqlock = common::Seqlock<BookSnapshot<depth>>;

template <std::size_t depth>
inline void capture(BookSnapshot<depth> &out, const equity::OrderBook &book,
                    const char (&symbol)[8], uint64_t event_seq) noexcept {
  out.event_seq = event_seq;
  std::memcpy(out.stock_id, symbol, 8);
  out.best_bid = book.best_bid();
  out.best_ask = book.best_ask();
  out.nb = book.top_bids(out.bids, static_cast<int>(depth));
  out.na = book.top_asks(out.asks, static_cast<int>(depth));
}

} // namespace core

#pragma once

#include "common/event.hpp"
#include "common/ipc/shm.hpp"
#include "core/core.hpp"
#include "core/snapshot/book_snapshot.hpp"
#include <algorithm>
#include <cstring>

namespace core {

// Build the resolved exec feed frame from the freshly-captured book snapshot plus the
// triggering event and any trade it produced, in place (normally straight into a
// claimed ring slot). The exec feed's depth is a prefix of the in-process snapshot,
// so we reuse its already-walked levels instead of walking the book a second time.
// Levels past nb/na are left as they were; consumers read only the counted ones.
template <std::size_t snap_depth>
inline void build_market_update(exec::MarketUpdate<exec::EXEC_DEPTH> &mu,
                                const common::Event &ev,
                                const BookSnapshot<snap_depth> &snap,
                                const equity::OrderBook::Trade &trade,
                                uint16_t instrument, uint8_t extra_flags) noexcept {
  static_assert(exec::EXEC_DEPTH <= snap_depth,
                "exec feed depth must fit within the in-process snapshot");
  // The ITCH instrument's tick is the feed's 4-dp unit, so prices widen as-is;
  // only the empty-side sentinels need mapping.
  mu.event_seq = snap.event_seq;
  mu.event_time = ev.timestamp;
  mu.tsc_in = ev.tsc_in;
  mu.best_bid = snap.nb > 0 ? int64_t(snap.best_bid) : exec::EMPTY_BID;
  mu.best_ask = snap.na > 0 ? int64_t(snap.best_ask) : exec::EMPTY_ASK;
  mu.trade_price = trade.price;
  mu.trade_qty = trade.shares;
  mu.instrument = instrument;
  mu.trade_side = trade.side;
  mu.nb = static_cast<int16_t>(std::min(snap.nb, static_cast<int>(exec::EXEC_DEPTH)));
  mu.na = static_cast<int16_t>(std::min(snap.na, static_cast<int>(exec::EXEC_DEPTH)));
  // ITCH delivers one event per frame, so every frame ends its batch.
  uint8_t flags = exec::LAST_IN_BATCH | extra_flags;
  if (mu.nb > 0 && mu.na > 0)
    flags |= exec::TWO_SIDED;
  if (ev.message_type == 'Q')
    flags |= exec::CROSS;
  mu.flags = flags;
  mu._pad = 0;
  for (int i = 0; i < mu.nb; ++i)
    mu.bids[i] = {snap.bids[i].price, snap.bids[i].shares,
                  snap.bids[i].order_count, 0};
  for (int i = 0; i < mu.na; ++i)
    mu.asks[i] = {snap.asks[i].price, snap.asks[i].shares,
                  snap.asks[i].order_count, 0};
}

} // namespace core

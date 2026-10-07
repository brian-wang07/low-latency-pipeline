#pragma once

#include "common/event.hpp"
#include "core/core.hpp"
#include <cstdint>
#include <cstring>

namespace core {

// Applies ITCH events to the book of one subscribed symbol and resolves the trade
// each event produced. The book is created on the symbol's first A/F (the only
// messages carrying the ticker); events for other symbols are filtered out.
struct PrimaryFeed {
  equity::BookArray &engine;
  const char (&symbol)[8];
  uint64_t count = 0; // events applied to the book; the frames' event_seq

  // Returns the book the event touched, or nullptr if it was filtered out.
  equity::OrderBook *apply(const common::Event &ev,
                           equity::OrderBook::Trade &trade) noexcept {
    equity::OrderBook *book = nullptr;
    bool is_add = (ev.message_type == 'A' || ev.message_type == 'F');

    if (is_add) {
      // Subscription gate: only A/F carries the stock symbol on the wire.
      if (std::memcmp(ev.stock, symbol, 8) != 0)
        return nullptr;
      // Seed base_price below the symbol's first price so the in-range window
      // brackets where the book will trade; adds outside it are dropped. The
      // occupancy bitmap makes walk cost independent of where the populated
      // range sits, so this only sets the drop boundary, not performance. For
      // symbols cheaper than HALF the window, base clamps to 0.
      constexpr uint32_t HALF =
          equity::DEFAULT_LEVEL_COUNT / 2 * equity::PRICE_TICK;
      equity::Price base = ev.price > HALF ? ev.price - HALF : 0u;
      book = &engine.ensure(ev.stock_locate, ev.stock_locate, base);
    } else {
      book = engine.get(ev.stock_locate);
      if (!book)
        return nullptr;
    }

    ++count;

    switch (ev.message_type) {
    case 'A':
    case 'F':
      book->on_add(ev.order_ref_number, ev.side, ev.price, ev.shares);
      break;
    case 'E':
      trade = book->on_execute(ev.order_ref_number, ev.shares);
      break;
    case 'C':
      trade = book->on_execute_with_price(ev.order_ref_number, ev.shares,
                                          ev.price);
      break;
    case 'X':
      book->on_cancel(ev.order_ref_number, ev.shares);
      break;
    case 'D':
      book->on_delete(ev.order_ref_number);
      break;
    case 'U':
      book->on_replace(ev.order_ref_number, ev.new_order_ref_number, ev.price,
                       ev.shares);
      break;
    case 'P':
      // Non-displayed (hidden) trade: real flow, but it never touches the visible
      // book. Side/price/size come straight off the wire.
      trade = {ev.price, ev.shares, ev.side};
      break;
    case 'Q':
      // Auction cross print: no book effect and no aggressor side. The frame
      // builder flags it CROSS so trade_side is not read.
      trade = {ev.price, ev.shares, common::Side::Buy};
      break;
    }
    return book;
  }
};

} // namespace core

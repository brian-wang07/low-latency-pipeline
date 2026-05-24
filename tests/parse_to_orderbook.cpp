#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <thread>

#include "common/event.hpp"
#include "common/ipc/exchange_shm.hpp"
#include "core/core.hpp"
#include "exchange/itch/itch_parser.hpp"

// ITCH stock names are space-padded to 8 bytes. The render target — every
// other subscribed ticker still gets its book built, just not drawn here.
static constexpr char PRIMARY[8] = {'T', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' '};
static constexpr int DEPTH = 15;

static void render(const core::equity::OrderBook &book, uint64_t event_count) {
  core::equity::OrderBook::Level bids[DEPTH];
  core::equity::OrderBook::Level asks[DEPTH];
  int nb = book.top_bids(bids, DEPTH);
  int na = book.top_asks(asks, DEPTH);

  std::fputs("\033[H\033[2J", stdout);
  std::printf("%.8s  events=%lu\n\n", PRIMARY,
              static_cast<unsigned long>(event_count));
  std::printf("%12s %12s   ||   %-12s %-12s\n", "Bid Qty", "Bid Price",
              "Ask Price", "Ask Qty");
  std::printf("%12s %12s   ||   %-12s %-12s\n", "------------", "------------",
              "------------", "------------");
  for (int i = 0; i < DEPTH; ++i) {
    if (i < nb)
      std::printf("%12u %12.4f", bids[i].shares, bids[i].price / 10000.0);
    else
      std::printf("%12s %12s", "", "");
    std::fputs("   ||   ", stdout);
    if (i < na)
      std::printf("%-12.4f %-12u\n", asks[i].price / 10000.0, asks[i].shares);
    else
      std::printf("%-12s %-12s\n", "", "");
  }
  std::fflush(stdout);
}

int main() {
  const char *path = "../itch_feed/S071321-v50.txt";
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  auto ring = std::make_unique<exchange::ExchangeRing>();
  std::atomic<bool> done{false};

  std::thread producer([&] {
    ItchParser parser(fd, ring.get());
    while (!parser.done())
      parser.next();
    done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    // Static so the 512 KB pointer table lives in BSS (heap holds active books).
    static core::equity::BookArray engine;

    // Captured from the first A/F we see for PRIMARY. Locate is the only
    // identifier present on E/C/X/D/U messages.
    uint16_t primary_locate = 0;
    bool primary_locked = false;
    uint64_t primary_count = 0;

    // Redraws can't keep up with raw event rate; cap at ~30 fps.
    auto last_draw = std::chrono::steady_clock::now();
    const auto frame = std::chrono::milliseconds(33);
    auto maybe_draw = [&] {
      if (!primary_locked)
        return;
      auto now = std::chrono::steady_clock::now();
      if (now - last_draw < frame)
        return;
      const core::equity::OrderBook *book = engine.get(primary_locate);
      if (book && book->initialized())
        render(*book, primary_count);
      last_draw = now;
    };

    auto process = [&](const common::Event &ev) {
      core::equity::OrderBook *book = nullptr;
      bool is_add = (ev.message_type == 'A' || ev.message_type == 'F');

      if (is_add) {
        // Subscription gate: only A/F carries the stock symbol on the wire.
        // (Extend here to subscribe more tickers.)
        if (memcmp(ev.stock, PRIMARY, 8) != 0)
          return;
        // Anchor base 32 768 ticks (~$3.28) below first seen price.
        core::equity::Price base = ev.price > 32768u ? ev.price - 32768u : 0u;
        book = &engine.ensure(ev.stock_locate, ev.stock_locate, base);
        if (!primary_locked) {
          primary_locate = ev.stock_locate;
          primary_locked = true;
        }
      } else {
        // E/C/X/D/U: locate is the only stock identifier on the wire.
        book = engine.get(ev.stock_locate);
        if (!book)
          return;
      }

      ++primary_count;

      switch (ev.message_type) {
      case 'A':
      case 'F':
        book->on_add(ev.order_ref_number, ev.side, ev.price, ev.shares);
        break;
      case 'E':
        book->on_execute(ev.order_ref_number, ev.shares);
        break;
      case 'C':
        book->on_execute_with_price(ev.order_ref_number, ev.shares, ev.price);
        break;
      case 'X':
        book->on_cancel(ev.order_ref_number, ev.shares);
        break;
      case 'D':
        book->on_delete(ev.order_ref_number);
        break;
      case 'U':
        book->on_replace(ev.order_ref_number, ev.new_order_ref_number,
                         ev.price, ev.shares);
        break;
      }

      maybe_draw();
    };

    common::Event ev;
    while (!done.load(std::memory_order_acquire)) {
      if (ring->try_pop(ev))
        process(ev);
    }
    while (ring->try_pop(ev))
      process(ev);

    if (!primary_locked) {
      std::printf("No %.8s events seen\n", PRIMARY);
      return;
    }
    const core::equity::OrderBook *book = engine.get(primary_locate);
    if (book)
      render(*book, primary_count);
  });

  producer.join();
  consumer.join();
  return 0;
}

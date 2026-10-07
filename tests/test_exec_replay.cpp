// Deterministic in-process replay of the exec feed: parser -> book -> frame builder
// -> MarketView -> strategy over the whole feed, with no shm ring (so no drops). Hashes everything exec can observe; the digest is pinned so frame
// or view changes that should be behavior-preserving are proven to be.
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <thread>

#include "check.hpp"
#include "common/ipc/shm.hpp"
#include "core/market_update_builder.hpp"
#include "core/primary_feed.hpp"
#include "core/snapshot/book_snapshot.hpp"
#include "exchange/itch/itch_parser.hpp"
#include "exec/market_view.hpp"
#include "exec/strategies/dummy_quoter.hpp"
#include "exec/strategy.hpp"

static constexpr char SYMBOL[8] = {'N', 'V', 'D', 'A', ' ', ' ', ' ', ' '};
// Re-pin only for an intended behavior change, and say why here.
// 0x605af753ab83cdd8: EXEC_DEPTH 5 -> 10 (the int64 widening alone kept 0x4007cf1c29a08c26).
// 0x2ccba671122fdfc0: frame flags hashed, ITCH 'Q' crosses carried as trades (+2).
static constexpr uint64_t EXPECTED_DIGEST = 0x2ccba671122fdfc0;

struct Fnv1a {
  uint64_t h = 14695981039346656037ull;
  void add(uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 1099511628211ull;
    }
  }
  void add_f64(double d) noexcept {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof(bits));
    add(bits);
  }
};

int main(int argc, char *argv[]) {
  const char *path = (argc > 1) ? argv[1] : "../itch_feed/S071321-v50.txt";
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  auto ring = std::make_unique<core::CoreRing>();
  std::atomic<bool> parsed_all{false};
  std::thread producer([&] {
    ItchParser parser(fd, ring.get());
    while (!parser.done())
      parser.next();
    parsed_all.store(true, std::memory_order_release);
  });

  static core::equity::BookArray engine;
  core::PrimaryFeed feed{engine, SYMBOL};
  static core::BookSnapshot<dashboard::DASH_DEPTH> snap{};
  exec::MarketView view;
  exec::StrategyContext ctx(view);
  exec::DummyQuoter quoter;

  Fnv1a digest;
  exec::MarketUpdate<exec::EXEC_DEPTH> mu{};
  uint64_t events = 0, frames = 0, trades = 0, crosses = 0;
  common::Event ev;
  for (;;) {
    if (!ring->try_pop(ev)) {
      if (parsed_all.load(std::memory_order_acquire) && ring->size() == 0)
        break;
      continue;
    }
    ++events;
    core::equity::OrderBook::Trade trade{};
    core::equity::OrderBook *book = feed.apply(ev, trade);
    if (!book)
      continue;
    core::capture(snap, *book, SYMBOL, feed.count);
    core::build_market_update(mu, ev, snap, trade, 0, 0);
    view.on_update(mu);
    quoter.on_tick(ctx);

    ++frames;
    if (mu.trade_qty != 0)
      ++trades;
    if (mu.flags & exec::CROSS)
      ++crosses;
    // Frame contents, widened to 64 bits; an empty ask hashes as a fixed marker
    // so the sentinel's width doesn't matter.
    digest.add(mu.event_seq);
    digest.add(mu.event_time);
    digest.add(uint64_t(mu.best_bid));
    digest.add(mu.na > 0 ? uint64_t(mu.best_ask) : UINT64_MAX);
    digest.add(uint64_t(mu.trade_price));
    digest.add(uint64_t(mu.trade_qty));
    digest.add(uint64_t(int64_t(mu.trade_side)));
    digest.add(uint64_t(int64_t(mu.nb)));
    digest.add(uint64_t(int64_t(mu.na)));
    digest.add(uint64_t(mu.flags));
    for (int i = 0; i < mu.nb; ++i) {
      digest.add(uint64_t(mu.bids[i].price));
      digest.add(uint64_t(mu.bids[i].qty));
      digest.add(uint64_t(mu.bids[i].order_count));
    }
    for (int i = 0; i < mu.na; ++i) {
      digest.add(uint64_t(mu.asks[i].price));
      digest.add(uint64_t(mu.asks[i].qty));
      digest.add(uint64_t(mu.asks[i].order_count));
    }
    // Derived view values, only where defined.
    if (view.two_sided()) {
      digest.add_f64(view.mid());
      digest.add_f64(view.microprice());
      digest.add_f64(view.imbalance());
    }
    digest.add_f64(view.realized_vol());
    digest.add(ctx.orders_submitted());
  }
  producer.join();

  std::printf("events=%" PRIu64 " frames=%" PRIu64 " trades=%" PRIu64
              " crosses=%" PRIu64 " orders=%" PRIu64 " gaps=%" PRIu64
              " digest=0x%016" PRIx64 "\n",
              events, frames, trades, crosses, ctx.orders_submitted(),
              view.frame_gaps(), digest.h);
  CHECK(events > 0);
  CHECK(frames > 0);
  CHECK(view.frame_gaps() == 0);
  CHECK(digest.h == EXPECTED_DIGEST);
  return check_summary();
}

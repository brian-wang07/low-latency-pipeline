#include "check.hpp"
#include "common/ipc/shm.hpp"
#include "exec/market_view.hpp"

using Frame = exec::MarketUpdate<exec::EXEC_DEPTH>;

static Frame frame(uint64_t seq, int64_t bid, int64_t ask, uint8_t flags = 0) {
  Frame f{};
  f.event_seq = seq;
  f.best_bid = bid;
  f.best_ask = ask;
  f.nb = f.na = 1;
  f.bids[0] = {bid, 100, 1, 0};
  f.asks[0] = {ask, 100, 1, 0};
  f.flags = flags;
  return f;
}

static void test_gaps_and_resets() {
  exec::MarketView v;
  v.on_update(frame(1, 1000, 1002));
  CHECK(!v.feed_reset());
  v.on_update(frame(2, 1000, 1002));
  v.on_update(frame(3, 1001, 1003));
  CHECK(!v.feed_reset());
  CHECK(v.frame_gaps() == 0);

  v.on_update(frame(6, 1001, 1003)); // 4 and 5 dropped
  CHECK(v.feed_reset());
  CHECK(v.frame_gaps() == 2);

  v.on_update(frame(7, 1001, 1003, exec::FEED_RESET));
  CHECK(v.feed_reset());
  CHECK(v.frame_gaps() == 2);

  v.on_update(frame(8, 1001, 1003));
  CHECK(!v.feed_reset());

  v.on_update(frame(1, 1001, 1003)); // producer restarted
  CHECK(v.feed_reset());
  CHECK(v.frame_gaps() == 2);
}

// A price jump across a gap must not be folded into realized vol.
static void test_vol_skips_gap() {
  exec::MarketView v;
  v.on_update(frame(1, 1000, 1002));
  v.on_update(frame(2, 1000, 1002));
  const double before = v.realized_vol();
  v.on_update(frame(10, 5000, 5002));
  CHECK(v.realized_vol() == before);
  v.on_update(frame(11, 5000, 5002));
  CHECK(v.realized_vol() < before + 1e-9);
}

int main() {
  test_gaps_and_resets();
  test_vol_skips_gap();
  return check_summary();
}

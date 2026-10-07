// Hot thread: drives the orderbook off the exchange ring and publishes a
// fixed-size frame to the seqlock after each event. All rendering /
// downstream work happens on the snapshotter thread, never here.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "common/config.hpp"
#include "common/config_file.hpp"
#include "common/histogram.hpp"
#include "common/ipc/dashboard_snapshot.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
#include "common/platform/tsc.hpp"
#include "common/seqlock.hpp"
#include "core/core.hpp"
#include "core/market_update_builder.hpp"
#include "core/primary_feed.hpp"
#include "core/snapshot/book_snapshot.hpp"
#include "core/snapshot/book_snapshotter.hpp"

struct LatencyStats {
  common::Histogram transit;
  common::Histogram process;
  common::Histogram e2e;
};
static_assert(std::is_trivially_copyable_v<LatencyStats>);

static volatile std::sig_atomic_t shutdown_flag{0};
static void on_signal(int) { shutdown_flag = 1; }

// ITCH tickers are space-padded to 8 bytes. The first of `symbols` is the book
// this core builds and publishes; default TQQQ.
static char PRIMARY[8] = {'T', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' '};

static void set_primary(const char *symbols) noexcept {
  std::memset(PRIMARY, ' ', sizeof(PRIMARY));
  for (int i = 0; i < 8 && symbols[i] && symbols[i] != ','; ++i)
    PRIMARY[i] = symbols[i];
  if (std::strchr(symbols, ','))
    std::fprintf(stderr, "core_main: only the first of symbols=%s is traded\n",
                 symbols);
}

// Book-frame depth is tied to the dashboard wire contract so the publisher can
// copy levels 1:1 into the shared-ring frame.
static constexpr std::size_t DEPTH = dashboard::DASH_DEPTH;

// Snapshotter callback. Runs on the snapshotter thread only (off the hot path).
// Converts the in-process book frame into a dashboard::Snapshot, derives the
// display analytics + pipeline-health metrics, and publishes to the shared
// core->dashboard ring. Drops the frame when the ring is full (the dashboard
// drains at ~60fps vs. our ~30Hz publish rate, so this is rare and harmless).
struct DashboardPublisher {
  dashboard::DashboardRing *out;      // producer end of the shared ring
  const core::CoreRing *in;           // input ring, read for occupancy only
  common::Seqlock<LatencyStats> *lat; // e2e latency source (multi-reader)
  double tsc_per_ns;
  uint16_t instrument;
  int8_t price_exp; // the instrument's tick exponent

  // EMA smoothing per snapshot frame (~30Hz) -> ~0.7s time constant.
  static constexpr double EMA_ALPHA = 0.05;

  // running state carried across frames
  bool have_prev = false;
  uint64_t prev_seq = 0;
  std::chrono::steady_clock::time_point prev_t{};
  bool ema_init = false;
  double ema = 0.0;

  void operator()(const core::BookSnapshot<DEPTH> &s) noexcept {
    dashboard::Snapshot<DEPTH> f{};

    f.event_seq = s.event_seq;
    f.instrument = instrument;
    f.price_exp = price_exp;
    f.nb = s.nb;
    f.na = s.na;

    const bool have_bid = s.nb > 0 && s.best_bid != 0u;
    const bool have_ask = s.na > 0 && s.best_ask != UINT32_MAX;
    f.best_bid = have_bid ? int64_t(s.best_bid) : 0;
    f.best_ask = have_ask ? int64_t(s.best_ask) : 0;
    f.spread = (have_bid && have_ask && s.best_ask >= s.best_bid)
                   ? int64_t(s.best_ask - s.best_bid)
                   : 0;

    int64_t tot_bid = 0, tot_ask = 0;
    for (int i = 0; i < s.nb; ++i) {
      f.bids[i] = {s.bids[i].price, s.bids[i].shares, s.bids[i].order_count, 0};
      tot_bid += s.bids[i].shares;
    }
    for (int i = 0; i < s.na; ++i) {
      f.asks[i] = {s.asks[i].price, s.asks[i].shares, s.asks[i].order_count, 0};
      tot_ask += s.asks[i].shares;
    }
    f.total_bid_qty = tot_bid;
    f.total_ask_qty = tot_ask;

    // depth imbalance over captured levels, in [-1, 1]
    const double tot = double(tot_bid) + double(tot_ask);
    f.imbalance = tot > 0.0 ? (double(tot_bid) - double(tot_ask)) / tot : 0.0;

    // volume-weighted mid (microprice): best prices weighted by the *opposite*
    // side's size, so it leans toward the thinner side. Display units.
    const double bid_px = dashboard::to_display(f.best_bid, price_exp);
    const double ask_px = dashboard::to_display(f.best_ask, price_exp);
    double vwmid;
    if (have_bid && have_ask) {
      const double bq = double(s.bids[0].shares);
      const double aq = double(s.asks[0].shares);
      const double q = bq + aq;
      vwmid =
          q > 0.0 ? (bid_px * aq + ask_px * bq) / q : (bid_px + ask_px) * 0.5;
    } else if (have_bid) {
      vwmid = bid_px;
    } else if (have_ask) {
      vwmid = ask_px;
    } else {
      vwmid = 0.0;
    }
    f.vwmid = vwmid;

    // EMA of the vwmid across frames (display units)
    if (have_bid || have_ask) {
      ema = ema_init ? EMA_ALPHA * vwmid + (1.0 - EMA_ALPHA) * ema : vwmid;
      ema_init = true;
    }
    f.ema = ema;

    // tick rate: change in cumulative event count over wall time between frames
    const auto now = std::chrono::steady_clock::now();
    if (have_prev) {
      const double dt = std::chrono::duration<double>(now - prev_t).count();
      if (dt > 0.0)
        f.tick_rate = double(s.event_seq - prev_seq) / dt;
    }
    prev_t = now;
    prev_seq = s.event_seq;
    have_prev = true;

    // pipeline health: input-ring fill ratio + e2e latency percentiles
    const double occ = double(in->size()) / double(core::CORE_RING_CAPACITY);
    f.ring_occupancy = occ > 1.0 ? 1.0 : occ;
    LatencyStats ls;
    if (lat->try_load(ls) && ls.e2e.count > 0) {
      f.latency_p99_ns = double(ls.e2e.percentile_cycles(0.99)) / tsc_per_ns;
      f.latency_p999_ns = double(ls.e2e.percentile_cycles(0.999)) / tsc_per_ns;
    }

    (void)out->try_push(f); // drop on full
  }
};

int main(int argc, char **argv) {
  static common::Config cfg;
  if (argc < 2 || !cfg.load_from_args(argc, argv))
    return 2;
  set_primary(cfg.get_str("symbols", "TQQQ"));
  // 0 replays the whole feed.
  const int64_t max_events_cfg = cfg.get_i64("max_events", 0);
  const uint64_t max_events =
      max_events_cfg > 0 ? uint64_t(max_events_cfg) : UINT64_MAX;

  ShmSegment shm;
  ipc::PipelineShm *p = ipc::attach_pipeline(argv[1], shm, "core_main");

  core::CoreRing *ring = &p->exchange_to_core;
  exec::FeedRing *feed_ring = &p->feed_to_exec;
  dashboard::DashboardRing *dash_ring = &p->core_to_dashboard;

  // Register the traded symbol. Prices keep the feed's 4-dp units (tick 1e-4) so
  // sub-penny midpoint prints survive; shares are lots of 1.
  ref::Instrument itch_row{};
  itch_row.tick_mant = 1;
  itch_row.tick_exp = -4;
  itch_row.lot_mant = 1;
  itch_row.min_qty_lots = 1;
  itch_row.price_precision = 4;
  itch_row.status = ref::ONLINE;
  for (int i = 0; i < 8 && PRIMARY[i] != ' '; ++i)
    itch_row.symbol[i] = PRIMARY[i];
  std::memcpy(itch_row.venue, "ITCH", 4);
  const uint16_t primary_instrument = p->instruments.add(itch_row);

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(int(cfg.get_i64("cores.core", config::HOT_CORE))))
    std::perror("pin_to_core hot");

  // calibrate
  double tsc_per_ns;
  calibrate_tsc(tsc_per_ns);

  // Latency log. Path overridable per-run via LAT_LOG=foo.log ./core ...
  const char *lat_log_path = std::getenv("LAT_LOG");
  if (!lat_log_path)
    lat_log_path = "latency.log";
  std::FILE *lat_log = std::fopen(lat_log_path, "w");
  if (!lat_log) {
    std::perror("fopen latency log");
    return 1;
  }
  static char lat_log_buf[1 << 16];
  std::setvbuf(lat_log, lat_log_buf, _IOFBF, sizeof(lat_log_buf));

  static core::equity::BookArray engine;
  engine.prewarm();

  // In-process bridge between hot thread (writer) and snapshotter thread
  static core::BookSeqlock<DEPTH> book_seq;
  core::BookSnapshot<DEPTH> scratch{};

  core::PrimaryFeed feed{engine, PRIMARY};

  // Hot-thread counters, published to shm on the PUB_MASK cadence. A full feed ring
  // drops the frame (never blocks) and flags the next one FEED_RESET.
  stats::FeedStats feed_stats{};
  feed_stats.mode = stats::FEED_ITCH;
  bool pending_reset = false;

  auto process = [&](const common::Event &ev) {
    core::equity::OrderBook::Trade trade{};
    core::equity::OrderBook *book = feed.apply(ev, trade);
    if (!book)
      return;

    // Exec frame first, built in place in the ring slot, then the book snapshot for
    // the dashboard so strategy latency doesn't wait on the dashboard copy.
    core::capture(scratch, *book, PRIMARY, feed.count);
    if (auto *slot = feed_ring->try_claim()) {
      core::build_market_update(*slot, ev, scratch, trade, primary_instrument,
                                pending_reset ? exec::FEED_RESET : 0);
      feed_ring->publish();
      ++feed_stats.frames_pushed;
      pending_reset = false;
    } else {
      ++feed_stats.frames_dropped;
      pending_reset = true;
    }
    book_seq.store(scratch);
  };

  auto publish_feed_stats = [&](uint64_t events_in) {
    feed_stats.events_in = events_in;
    feed_stats.heartbeat_tsc = read_tsc();
    p->feed_stats.store(feed_stats);
  };

  // Hot thread publishes cumulative histograms here; both the snapshotter (for
  // the dashboard's latency panel) and the dumper thread read it.
  static common::Seqlock<LatencyStats> lat_seq;

  // snapshot thread: publishes dashboard frames to the shared ring at ~30Hz
  std::atomic<bool> snap_shutdown{false};
  DashboardPublisher publisher{dash_ring,  ring,
                               &lat_seq,   tsc_per_ns,
                               primary_instrument, itch_row.tick_exp};
  std::thread snap_thread([&] {
    core::run_snapshotter<DEPTH>(book_seq,
                                 int(cfg.get_i64("cores.snapshot", config::SNAPSHOT_CORE)),
                                 std::chrono::milliseconds(33), snap_shutdown,
                                 publisher);
  });

  std::atomic<bool> lat_shutdown{false};
  std::thread lat_thread([&] {
    if (!pin_to_core(int(cfg.get_i64("cores.lat_dump", config::LAT_DUMP_CORE))))
      std::perror("pin_to_core lat dump");
    LatencyStats local{};
    while (!lat_shutdown.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (lat_seq.try_load(local) && local.e2e.count > 0) {
        local.transit.dump(lat_log, "transit", tsc_per_ns);
        local.process.dump(lat_log, "process", tsc_per_ns);
        local.e2e.dump(lat_log, "e2e", tsc_per_ns);
        std::fprintf(lat_log, "[drops] %llu\n",
                     static_cast<unsigned long long>(core::equity::g_drops.load(
                         std::memory_order_relaxed)));
      }
    }
  });

  common::Event ev;
  common::Histogram hist_ipc;
  common::Histogram hist_core;
  common::Histogram hist_e2e;
  constexpr uint64_t PUB_MASK = (1ull << 16) - 1; // publish every 65k events
  uint64_t n{0};
  while (!shutdown_flag && n < max_events) {
    if (ring->try_pop(ev)) {
      uint64_t t1 = read_tsc();
      process(ev);
      uint64_t t3 = read_tsc();
      hist_ipc.record(t1 - ev.tsc_in);
      hist_core.record(t3 - t1);
      hist_e2e.record(t3 - ev.tsc_in);
      if ((++n & PUB_MASK) == 0) {
        LatencyStats snap{hist_ipc, hist_core, hist_e2e};
        lat_seq.store(snap);
        publish_feed_stats(n);
      }
    }
  }
  uint64_t drained = 0; // kept out of n so benchmark counts stay comparable
  while (ring->try_pop(ev)) {
    process(ev);
    ++drained;
  }
  publish_feed_stats(n + drained);

  lat_shutdown.store(true, std::memory_order_release);
  lat_thread.join();

  snap_shutdown.store(true, std::memory_order_release);
  snap_thread.join();

  std::fprintf(lat_log,
               "\n=== aggregated (n=%llu drops=%llu frames=%llu "
               "frames_dropped=%llu) ===\n",
               static_cast<unsigned long long>(n),
               static_cast<unsigned long long>(
                   core::equity::g_drops.load(std::memory_order_relaxed)),
               static_cast<unsigned long long>(feed_stats.frames_pushed),
               static_cast<unsigned long long>(feed_stats.frames_dropped));
  hist_ipc.dump(lat_log, "transit", tsc_per_ns);
  hist_core.dump(lat_log, "process", tsc_per_ns);
  hist_e2e.dump(lat_log, "e2e", tsc_per_ns);
  std::fprintf(lat_log, "\n=== full distribution ===\n");
  hist_ipc.dump_full(lat_log, "transit", tsc_per_ns);
  hist_core.dump_full(lat_log, "process", tsc_per_ns);
  hist_e2e.dump_full(lat_log, "e2e", tsc_per_ns);
  std::fclose(lat_log);

  if (feed.count == 0)
    std::printf("No %.8s events seen\n", PRIMARY);
  return 0;
}

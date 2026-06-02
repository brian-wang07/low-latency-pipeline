// Hot thread: drives the orderbook off the exchange ring and publishes a
// fixed-size frame to the seqlock after each event. All rendering /
// downstream work happens on the snapshotter thread, never here.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "common/config.hpp"
#include "common/histogram.hpp"
#include "common/ipc/dashboard_snapshot.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
#include "common/platform/spin_pause.hpp"
#include "common/platform/tsc.hpp"
#include "common/seqlock.hpp"
#include "core/core.hpp"
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

static constexpr char PRIMARY[8] = {'T', 'Q', 'Q', 'Q', ' ', ' ', ' ', ' '};

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
    std::memcpy(f.stock_id, s.stock_id, 8);
    f.best_bid = s.best_bid;
    f.best_ask = s.best_ask;
    f.nb = s.nb;
    f.na = s.na;

    const bool have_bid = s.nb > 0 && s.best_bid != 0u;
    const bool have_ask = s.na > 0 && s.best_ask != UINT32_MAX;
    f.spread = (have_bid && have_ask && s.best_ask >= s.best_bid)
                   ? (s.best_ask - s.best_bid)
                   : 0u;

    uint64_t tot_bid = 0, tot_ask = 0;
    for (int i = 0; i < s.nb; ++i) {
      f.bids[i] = {s.bids[i].price, s.bids[i].shares, s.bids[i].order_count};
      tot_bid += s.bids[i].shares;
    }
    for (int i = 0; i < s.na; ++i) {
      f.asks[i] = {s.asks[i].price, s.asks[i].shares, s.asks[i].order_count};
      tot_ask += s.asks[i].shares;
    }
    f.total_bid_qty = tot_bid;
    f.total_ask_qty = tot_ask;

    // depth imbalance over captured levels, in [-1, 1]
    const double tot = double(tot_bid) + double(tot_ask);
    f.imbalance = tot > 0.0 ? (double(tot_bid) - double(tot_ask)) / tot : 0.0;

    // volume-weighted mid (microprice): best prices weighted by the *opposite*
    // side's size, so it leans toward the thinner side. Display units.
    const double bid_px = dashboard::to_display(s.best_bid);
    const double ask_px = dashboard::to_display(s.best_ask);
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
  if (argc != 2)
    std::abort();

  int shm_fd = std::stoi(argv[1]);

  ShmSegment shm;
  if (!shm.attach(shm_fd, ipc::SHM_SIZE))
    std::abort();
  auto *p = shm.as<ipc::PipelineShm>();
  while (p->header.magic.load(std::memory_order_acquire) == 0) {
    SPIN_PAUSE();
  }
  if (p->header.magic != ipc::MAGIC)
    std::abort();

  core::CoreRing *ring = &p->exchange_to_core;
  dashboard::DashboardRing *dash_ring = &p->core_to_dashboard;

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(config::HOT_CORE))
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

  uint16_t primary_locate = 0;
  bool primary_locked = false;
  uint64_t primary_count = 0;

  auto process = [&](const common::Event &ev) {
    core::equity::OrderBook *book = nullptr;
    bool is_add = (ev.message_type == 'A' || ev.message_type == 'F');

    if (is_add) {
      // Subscription gate: only A/F carries the stock symbol on the wire.
      if (std::memcmp(ev.stock, PRIMARY, 8) != 0)
        return;
      // Seed base_price below the symbol's first price so the in-range window
      // brackets where the book will trade; adds outside it are dropped. The
      // occupancy bitmap makes walk cost independent of where the populated
      // range sits, so this only sets the drop boundary, not performance. For
      // symbols cheaper than HALF the window, base clamps to 0.
      constexpr uint32_t HALF =
          core::equity::DEFAULT_LEVEL_COUNT / 2 * core::equity::PRICE_TICK;
      core::equity::Price base = ev.price > HALF ? ev.price - HALF : 0u;
      book = &engine.ensure(ev.stock_locate, ev.stock_locate, base);
      if (!primary_locked) {
        primary_locate = ev.stock_locate;
        primary_locked = true;
      }
    } else {
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
      book->on_replace(ev.order_ref_number, ev.new_order_ref_number, ev.price,
                       ev.shares);
      break;
    }

    // attempt to publish frame
    core::capture(scratch, *book, PRIMARY, primary_count);
    book_seq.store(scratch);
  };

  // Hot thread publishes cumulative histograms here; both the snapshotter (for
  // the dashboard's latency panel) and the dumper thread read it.
  static common::Seqlock<LatencyStats> lat_seq;

  // snapshot thread: publishes dashboard frames to the shared ring at ~30Hz
  std::atomic<bool> snap_shutdown{false};
  DashboardPublisher publisher{dash_ring, ring, &lat_seq, tsc_per_ns};
  std::thread snap_thread([&] {
    core::run_snapshotter<DEPTH>(book_seq, config::SNAPSHOT_CORE,
                                 std::chrono::milliseconds(33), snap_shutdown,
                                 publisher);
  });

  std::atomic<bool> lat_shutdown{false};
  std::thread lat_thread([&] {
    if (!pin_to_core(config::LAT_DUMP_CORE))
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
  // constexpr uint64_t MAX_EVENTS = 20'000'000;
  uint64_t n{0};
  while (!shutdown_flag
         // && n < MAX_EVENTS
  ) {
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
      }
    }
  }
  while (ring->try_pop(ev))
    process(ev);

  lat_shutdown.store(true, std::memory_order_release);
  lat_thread.join();

  snap_shutdown.store(true, std::memory_order_release);
  snap_thread.join();

  std::fprintf(lat_log, "\n=== aggregated (n=%llu drops=%llu) ===\n",
               static_cast<unsigned long long>(n),
               static_cast<unsigned long long>(
                   core::equity::g_drops.load(std::memory_order_relaxed)));
  hist_ipc.dump(lat_log, "transit", tsc_per_ns);
  hist_core.dump(lat_log, "process", tsc_per_ns);
  hist_e2e.dump(lat_log, "e2e", tsc_per_ns);
  std::fprintf(lat_log, "\n=== full distribution ===\n");
  hist_ipc.dump_full(lat_log, "transit", tsc_per_ns);
  hist_core.dump_full(lat_log, "process", tsc_per_ns);
  hist_e2e.dump_full(lat_log, "e2e", tsc_per_ns);
  std::fclose(lat_log);

  if (!primary_locked)
    std::printf("No %.8s events seen\n", PRIMARY);
  return 0;
}

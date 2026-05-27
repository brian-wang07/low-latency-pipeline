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

#include "common/histogram.hpp"
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

static constexpr char PRIMARY[8] = {'N', 'V', 'D', 'A', ' ', ' ', ' ', ' '};
static constexpr std::size_t DEPTH = 15;
static constexpr int HOT_CORE = 4;
static constexpr int SNAPSHOT_CORE = 5;

// Snapshotter callback. Runs on the snapshotter thread only. Temporary
// stand-in until the dashboard process consumes from an SPSC ring.
static void render(const core::BookSnapshot<DEPTH> &snap) {
  std::fputs("\033[H\033[2J", stdout);
  std::printf("%.8s  events=%lu\n\n", snap.stock_id,
              static_cast<unsigned long>(snap.event_seq));
  std::printf("%12s %12s   ||   %-12s %-12s\n", "Bid Qty", "Bid Price",
              "Ask Price", "Ask Qty");
  std::printf("%12s %12s   ||   %-12s %-12s\n", "------------", "------------",
              "------------", "------------");
  for (std::size_t i = 0; i < DEPTH; ++i) {
    if (static_cast<int>(i) < snap.nb)
      std::printf("%12u %12.4f", snap.bids[i].shares,
                  snap.bids[i].price / 10000.0);
    else
      std::printf("%12s %12s", "", "");
    std::fputs("   ||   ", stdout);
    if (static_cast<int>(i) < snap.na)
      std::printf("%-12.4f %-12u\n", snap.asks[i].price / 10000.0,
                  snap.asks[i].shares);
    else
      std::printf("%-12s %-12s\n", "", "");
  }
  std::fflush(stdout);
}

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

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(HOT_CORE))
    std::perror("pin_to_core hot");

  // calibrate
  double tsc_per_ns;
  calibrate_tsc(tsc_per_ns);

  // Latency log. Path overridable per-run via LAT_LOG=foo.log ./core ...
  // Line-buffered so `tail -f` shows new dumps live.
  const char *lat_log_path = std::getenv("LAT_LOG");
  if (!lat_log_path)
    lat_log_path = "latency.log";
  std::FILE *lat_log = std::fopen(lat_log_path, "w");
  if (!lat_log) {
    std::perror("fopen latency log");
    return 1;
  }
  std::setvbuf(lat_log, nullptr, _IOLBF, 0);

  static core::equity::BookArray engine;

  // In-process bridge between hot thread (writer) and snapshotter thread
  // (reader). Not in shm — both threads live in this process.
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

  // snapshot thread
  std::atomic<bool> snap_shutdown{false};
  std::thread snap_thread([&] {
    core::run_snapshotter<DEPTH>(book_seq, SNAPSHOT_CORE,
                                 std::chrono::milliseconds(33), snap_shutdown,
                                 render);
  });

  // Hot thread publishes cumulative histograms here; dumper thread reads.
  static common::Seqlock<LatencyStats> lat_seq;
  std::atomic<bool> lat_shutdown{false};
  std::thread lat_thread([&] {
    LatencyStats local{};
    while (!lat_shutdown.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (lat_seq.try_load(local) && local.e2e.count > 0) {
        local.transit.dump(lat_log, "transit", tsc_per_ns);
        local.process.dump(lat_log, "process", tsc_per_ns);
        local.e2e.dump(lat_log, "e2e", tsc_per_ns);
      }
    }
  });

  common::Event ev;
  common::Histogram hist_ipc;
  common::Histogram hist_core;
  common::Histogram hist_e2e;
  constexpr uint64_t PUB_MASK = (1ull << 16) - 1; // publish every 65k events
  constexpr uint64_t MAX_EVENTS = 20'000'000;
  uint64_t n{0};
  while (!shutdown_flag && n < MAX_EVENTS) {
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

  std::fprintf(lat_log, "\n=== aggregated (n=%llu) ===\n",
               static_cast<unsigned long long>(n));
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

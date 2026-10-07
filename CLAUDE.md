# CLAUDE.md

Low-latency trading pipeline in C++20. Today it replays a NASDAQ ITCH 5.0 file through a
multi-process, shared-memory pipeline: parser → L3 order book → strategy host
plus an ImGui dashboard. The next phase (see `plan.md`) adds live crypto trading.

## Build, test, run

```
cmake -S . -B build && cmake --build build -j8     # Release by default
cd build && ./test_orderbook && ./test_level_bitmap && ./test_itch_parser
cd build && ./manager                              # runs the whole pipeline
```

- Tests are standalone executables with a hand-rolled `CHECK` macro (`tests/test_orderbook.cpp`),
  not ctest yet. `test_itch_parser` replays the full feed (~454M events) and takes a while.
- Run binaries from `build/`: the manager execs `./exchange_main`, `./core_main`,
  `./exec_main_$EXEC_STRATEGY` (default `logging`) and `./dashboard`, and the feed path is
  hardcoded as `../itch_feed/S071321-v50.txt`.
- `EXEC_LATENCY_NS` overrides the shadow fill model's latency budget (default 10 us event time);
  both are deleted in plan Phase 1.
- Shared memory is a 16 MiB memfd from the hugepage pool; needs `vm.nr_hugepages=20` and
  `vm.hugetlb_shm_group=1000` (see README). This machine has both configured.

## Benchmarks

core_main writes latency histograms to `build/latency.log` (override with `LAT_LOG=`).
`scripts/plot_latency.py [log] -o out.png` plots them. Each benchmark is recorded in
`benchmarks/benchmarks.md` under its commit hash with a plot in `benchmarks/plots/<hash>.png`
(20M messages, NVDA). The committed core_main has the 20M cap commented out and PRIMARY =
TQQQ; to benchmark, locally uncomment `MAX_EVENTS` (and its loop condition), set PRIMARY to
NVDA, run `EXEC_STRATEGY=logging ./manager` (exit code 1 at the cap is expected), then revert.
Standard conditions: fresh boot, no other processes running, and the feed warmed into page
cache first with `cat itch_feed/S071321-v50.txt > /dev/null`. Claude cannot reboot or idle the
machine, so a run made from a session is marked as not fully accurate in benchmarks.md.
The latest entry is 2cd81d8, the baseline for plan Phase 1; it was run from a Claude session,
not under standard conditions, and should be re-run. Record an entry for every hot-path
change so regressions are attributable.

## Architecture (current)

- `manager`: creates the shm segment, placement-news `ipc::PipelineShm`, release-stores the
  magic, forks/execs workers with the shm fd as argv[1], `PR_SET_PDEATHSIG`. Any worker exit is
  fatal and broadcasts SIGTERM to the process group.
- `exchange_main` (core 2): `ItchParser` writes `common::Event` (128 B) into `exchange_to_core`.
- `core_main` (hot thread core 4, snapshotter core 6, latency dump core 0): L3 `OrderBook` per
  `stock_locate` (robin-hood `OrderMap`, `PriceLevelArray` + radix-64 `OccupancyBitmap`,
  cached TOB). Publishes `exec::MarketUpdate<5>` to `core_to_exec` per event (silently dropped
  if full) and 15-level snapshots to the dashboard via seqlock → snapshotter → ring.
- `exec_main_<name>` (core 3): one binary per strategy in CMake's `EXEC_STRATEGIES`
  (`name:Type:header`), so `on_tick` inlines into `run_strategy<S>`. `MarketView` + shadow
  `FillModel` (being removed, see below); strategies satisfy the `exec::Strategy` concept.
- `dashboard`: ImGui/ImPlot reader of the snapshot ring.

The README's architecture diagram (matching engine, order loopback) is stale; the README is
updated in plan Phase 6.

## Hardware

6C/12T box, Fedora 43, SMT pairs (0,1) (2,3) ... (10,11). Boot params isolate CPUs 2-5
(`isolcpus nohz_full rcu_nocbs`) with IRQs on 0,1,6-11. Core assignments live in
`src/common/config.hpp`; EXEC_CORE 3 is deliberately the SMT sibling of EXCHANGE_CORE 2 because
only two physical cores are isolated. OpenSSL headers are not installed yet
(`sudo dnf install openssl-devel`, needed from plan Phase 3).

## Conventions

- Release flags: `-O3 -march=native -fno-exceptions -fno-rtti -fno-plt`, LTO. No exceptions
  anywhere: parse numbers with `strtol`/`strtoull`, never `std::stoi` (it terminates).
- Hot path: busy-polled, pinned, allocation-free, `noexcept`, stamped with `read_tsc()`. Static
  dispatch via templates and concepts; no virtuals on the hot path.
- Cross-process types are POD, trivially copyable, 64-byte aligned, with `static_assert`s on
  layout. Bump `ipc::VERSION` (currently 2) on any shm layout change.
- SPSC rings (`common::SpscRing`) with uint32 head/tail; seqlocks for single-writer state.
- Comments: lean, only where non-obvious. Match the surrounding style.
- `*.txt` is gitignored (the ITCH feed is .txt); use `.cfg` for config files.
- When asked for "scaffolding", an "outline" or "how to", answer in prose, not code.

## Next phase: plan.md

`plan.md` is the source of truth for the live-trading phase (7 phases: shm v3 contracts → exec
as trading engine on ITCH → TLS/WS/FIX transport → Coinbase sandbox → Kraken live → hot-swap and
hardening → benchmark-gated optimizations). Key decisions:

- No in-process fill model. Live and sandbox fills come only from venue execution reports
  (GatewayRouter); ITCH mode uses a NullRouter (acks, never fills) and serves as the pipeline
  latency benchmark. The shadow `FillModel` is deleted in Phase 1; do not reintroduce one.
- Strategy research on historic data uses hftbacktest (Python/numba, in `research/`, outside
  CMake): order-level queue model on Kraken level3 data, measured order latency, Kraken's
  percentage fee model. It has no rate-limit model, so research strategies use
  `research/rate_model.py`, kept in sync with the C++ rate model through shared test vectors.
  Data comes from `kraken_recorder` (unpinned tool, never inside a trading process) or Tardis.
  Promising strategies are ported to C++, and a calibration step compares backtests of live
  minimum-size sessions against actual fills.
- Fees dominate at retail Kraken tiers (a maker round trip costs on the order of 18 bp against
  a BTC/USD spread far below 1 bp), so every backtest uses the account's real fee tier.
- The operator is an Ontario retail trader. **Coinbase Exchange FIX sandbox is the test venue
  only** (production FIX is institutional-only). **Kraken spot WS v2 is the live venue**
  (OSC-registered). Hyperliquid is dropped (restricts Ontario). Never suggest VPN or proxy
  routing around venue or regulator restrictions.
- Kraken constraints are design inputs: spot only (no shorting; sells bounded by holdings),
  trade only BTC/ETH/BCH/LTC (exempt from the $30k net-acquisition cap), and a per-pair rate
  counter that heavily penalizes cancelling young orders (prefer amend, hold quotes longer).
  No Kraken spot sandbox: `validate=true` is the dry run.
- Real money only via Kraken, only with `live=1`, starting in PAUSE.
- ITCH prices use a 0.0001 price unit (tick_exp -4) in the int64 frames, not 0.01: sub-penny
  midpoint prints must survive. Live-mode cores: exec 4, gateway MD 2, gateway OE 3.
- API keys live in `.env` at the repo root (gitignored, 0600); never commit or print it.

Open item noted against the plan: Kraken WS token behavior after 15-minute expiry on an open
connection is unconfirmed.

## Known quirks in current code

- `common::Event` is 128 B, not 64 B (`_pad[2]` is misleading).
- `ItchParser::next()` back-pressures on `EXCHANGE_RING_CAPACITY` (4096) while writing the
  8192-slot core ring, so queue depth never exceeds 50%; it also widens tail to uint64_t.
- `core_main` drops exec frames silently when the exec ring is full; nothing counts it.
- Seqlock fence ordering is x86-only; histogram buckets are power-of-two.
- `build/plan.md` is an old, unrelated latency plan; the live plan is `plan.md` at the root.

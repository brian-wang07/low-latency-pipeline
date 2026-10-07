# CLAUDE.md

Low-latency trading pipeline in C++20. Today it replays a NASDAQ ITCH 5.0 file through a
multi-process, shared-memory pipeline: parser → L3 order book → strategy host
plus an ImGui dashboard. The next phase (see `plan.md`) adds live crypto trading.

## Build, test, run

```
cmake -S . -B build && cmake --build build -j8     # Release by default
cd build && ctest -LE slow                         # fast unit tests
cd build && ctest -L slow                          # full-feed replays, ~30 s each
cd build && ./manager ../configs/itch-null.cfg     # runs the whole pipeline
scripts/loopback_test.sh                           # end-to-end NullRouter checks, ~4 min
```

- Tests are standalone executables using the `CHECK` macro from `tests/check.hpp`, registered
  with ctest (run from `build/`). `test_itch_parser` and `test_exec_replay` (label `slow`)
  replay the full feed. `test_exec_replay` pins a digest of everything exec observes on NVDA;
  `test_wire_layout` pins `ipc::LAYOUT_HASH` and sizes. Re-pin either only for an intended
  change, with a note in the test (and a VERSION bump for released layouts).
- Running `./manager` from a tool: start it with `setsid` (it SIGTERMs its own process group)
  and find workers with `pidof`, not `pgrep -f`, which also matches the calling shell.
- Run binaries from `build/`. The manager takes a `.cfg` (`configs/itch-null.cfg` is the
  default; keys: `feed_path`, `symbols`, `max_events`, `strategy`, `dashboard`, `cores.*`,
  `risk.*`, `rate.*`/`kraken.*`, `null.balance.*`, `exec.*`, `auto_restart_exec`) and passes
  `--config=` to every worker. A repeated key overrides an earlier one, so a run variant is a
  copy of a cfg with lines appended (as `loopback_test.sh` does).
- `strategy=builtin:<name>` runs `./exec_main_<name>`; `strategy=./strategy_<name>.so` runs
  the generic `./exec_main`, which loads the module. To switch strategy without touching
  core_main: edit `strategy=` and `kill -USR1 <manager>`, or use the dashboard's swap field
  (exec exits 75 and the manager respawns it). exec writes `strategy_stats.log` on exit.
- Shared memory is a 16 MiB memfd from the hugepage pool; needs `vm.nr_hugepages=20` and
  `vm.hugetlb_shm_group=1000` (see README). This machine has both configured.

## Benchmarks

core_main writes latency histograms to `build/latency.log` (override with `LAT_LOG=`).
`scripts/plot_latency.py [log] -o out.png` plots them. Each benchmark is recorded in
`benchmarks/benchmarks.md` under its commit hash with a plot in `benchmarks/plots/<hash>.png`
(20M messages, NVDA): run `./manager ../configs/bench.cfg` from build/ (logging strategy,
`max_events=20_000_000`; exit code 0 at the cap). No source edits are needed any more.
Standard conditions: fresh boot, no other processes running, and the feed warmed into page
cache first with `cat itch_feed/S071321-v50.txt > /dev/null`. Claude cannot reboot or idle the
machine, so a run made from a session is marked as not fully accurate in benchmarks.md.
The latest entries (2cd81d8, the Phase 1 baseline, and 031fa98) were run from a Claude
session, not under standard conditions, and should be re-run. Record an entry for every hot-path
change so regressions are attributable.

## Architecture (current)

- `manager <cfg>`: creates the shm segment, placement-news `ipc::PipelineShm`, release-stores
  the magic, forks/execs workers (shm fd as argv[1], `--config=`), `PR_SET_PDEATHSIG`, then
  supervises: exchange/core exit 0 (feed done, `max_events`) shuts everything down
  gracefully; their death sets KILL|PAUSE then shuts down; exec exit 75 or SIGUSR1 restarts
  exec alone; exec death sets KILL and respawns only with `auto_restart_exec=1`; dashboard
  exit is ignored. Shutdown SIGTERMs the process group.
- `exchange_main` (core 2): `ItchParser` writes `common::Event` (128 B) into `exchange_to_core`.
- `core_main` (hot thread core 4, snapshotter core 6, latency dump core 0): L3 `OrderBook` per
  `stock_locate` (robin-hood `OrderMap`, `PriceLevelArray` + radix-64 `OccupancyBitmap`,
  cached TOB); `core::PrimaryFeed` applies events for the one subscribed symbol. Per event it
  builds `exec::MarketUpdate<10>` (576 B, int64 ticks/lots, instrument, flags) in place in
  `feed_to_exec`; a full ring drops the frame, counts it in `FeedStats` and flags the next one
  `FEED_RESET`. Then 15-level snapshots go to the dashboard via seqlock → snapshotter → ring.
  It registers its symbol in the shm `InstrumentTable` at startup.
- exec (core 3) is a trading engine: `ExecContext` (`src/exec/exec_context.hpp`) owns the
  `MarketView`, `Account` (avg-cost positions, spot balances with reservations, 1e-8 money
  units), `OrderTable` (dense rows + open-addressed index), `RiskGate` and `RateModel` (Kraken
  per-pair counter). Strategies write `template <class Ctx> void on_tick(Ctx &) noexcept`
  against `StrategyContext<Router>` (submit/replace/cancel/cancel_all, position, rate_budget;
  every request is risk-checked first) with optional on_start/on_stop/on_exec/on_timer hooks.
  `run_strategy<S, Router>` polls reports, handles one frame, and every timer period
  (`exec.timer_ms`) publishes ExecStats/PositionStats/BalanceStats/OpenOrders. ITCH mode uses
  `NullRouter` (acks, never fills) and the event-time clock (`exec.clock=event`).
- Strategy modules (`src/exec/module_abi.hpp`, `loader.cpp`): each `EXEC_STRATEGIES` entry
  (`name:Type:header`) builds `exec_main_<name>` (strategy built in) and `strategy_<name>.so`;
  both expose the same `ll_strategy_module_v1` table and compile the whole hot loop, so code
  is identical. The loader copies a .so into a memfd, rejects PT_TLS, checks ABI/layout
  hash/ISA, and pre-faults it; `scripts/check_strategy_so.sh` gates every module at build.
- shm v4 (`ipc::VERSION` 4) also carries `Control`, the order/report rings (no producer until
  the gateway) and the stats seqlocks. Workers attach through `ipc::attach_pipeline`, which
  checks magic, version and `LAYOUT_HASH` (`src/common/ipc/layout.hpp`).
- `dashboard`: ImGui/ImPlot; a status strip (exec/feed heartbeats, KILL/PAUSE buttons, swap
  field) above Market (book and plots), Trading (positions, balances, open orders,
  execution, rejects by reason) and Latency & Health tabs. Self-paced at ~60 fps without
  vsync (a hidden Wayland window blocks swaps forever).

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
  layout. Bump `ipc::VERSION` (currently 4) on any shm layout change.
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
  latency benchmark. The shadow `FillModel` was deleted in Phase 1; do not reintroduce one.
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
- core_main builds one book: only the first of `symbols` is traded.
- The Kraken amend cost is read as +1 plus the age penalty (conservative); confirm against
  Kraken's docs before Phase 5 (`src/exec/rate_model.hpp`, `tests/fixtures/kraken_rate_vectors.csv`).
- Exec state lives in zero-filled static storage; exec_main pre-faults it before the loop
  (first-touch faults put a 3-6 us cluster in tick-to-order before that).
- Seqlock fence ordering is x86-only; histogram buckets are power-of-two.
- `build/plan.md` is an old, unrelated latency plan; the live plan is `plan.md` at the root.

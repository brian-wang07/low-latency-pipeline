# Live crypto trading plan: gateway layer, strategy modules, dashboard metrics

## Context

The pipeline replays NASDAQ ITCH from a file. exchange_main parses into a hugepage shared-memory
SPSC ring, core_main rebuilds an L3 book on a pinned hot thread and publishes 15-level snapshots
to the ImGui dashboard. Commit 2cd81d8 ("update") added the strategy process: core_main now also
publishes a resolved market-data frame (exec::MarketUpdate, top 5 levels per side plus the trade
the event produced) to a core_to_exec ring on every event, and exec_main busy-polls it, keeps a
MarketView and a shadow FillModel, and calls the strategy's on_tick. Orders are shadow-only
today; DummyQuoter logs inventory and PnL to stderr.

Goal: trade on live crypto venues through a venue-agnostic gateway, swap strategies quickly at
run time without losing hot-path performance, and show the metrics that matter for live trading
on the dashboard. Two venues with distinct roles: the Coinbase Exchange public sandbox (FIX 5.0
SP2) is the test venue and never trades real money; Kraken spot (JSON over WSS v2) is the live
venue. Network RTT (ms) dwarfs the pipeline (~200 ns) but the internal path stays
allocation-free, busy-polled and fully inlined.

Venue choice is driven by jurisdiction: the operator is a retail trader in Ontario. Coinbase
Exchange FIX is institutional-only (Advanced Trade, the retail product, has no FIX) but its
sandbox is open to anyone. Hyperliquid restricts Ontario in its Terms of Use and is dropped; no
VPN or proxy routing around venue or regulator restrictions. Kraken's Canadian entity (Payward
Canada) is an OSC-registered restricted dealer (April 2025) and serves Ontario retail.

Verified venue facts (Oct 2026 docs):

- Coinbase Exchange sandbox is free and public. Endpoints:
  fix-ord.sandbox.exchange.coinbase.com:6121 (order entry),
  fix-md.sandbox.exchange.coinbase.com:6121 (market data, snapshot enabled) and :6122 (snapshot
  disabled). Native TLS. BeginString FIXT.1.1, DefaultApplVerID(1137)=9. Logon(35=A): 553=API key,
  554=passphrase, 95/96 = base64(HMAC-SHA256(base64decode(secret),
  SendingTime|MsgType|MsgSeqNum|SenderCompID|TargetCompID|Passphrase joined by SOH)), 8013=Y cancels
  orders on disconnect, HeartBtInt <= 30 s. ClOrdID(11) must be a lowercase UUID v4 string. Market
  data: MarketDepth(264) 1=L1, 10=L2 (top-10 aggregated), 0 or omitted = L3 (per-order IDs); 35=W
  snapshot then 35=X incrementals with RptSeq(83), MDUpdateAction(279), MDEntryType(269). Weekly
  forced logout Saturday 13:00 ET. The sandbox has its own web login and API keys (separate from
  production), unlimited fake funds, and a subset of production books. Production FIX is never
  configured.
- Kraken spot WS v2: public wss://ws.kraken.com/v2 (market data), private
  wss://ws-auth.kraken.com/v2 (orders and executions). Private access needs a token from REST
  POST https://api.kraken.com/0/private/GetWebSocketsToken, signed API-Sign =
  base64(HMAC-SHA512(base64decode(secret), uri_path ++ SHA256(nonce ++ urlencoded_post_data)))
  with API-Key header; tokens are valid 15 minutes from creation (open item: confirm whether an
  established private connection survives expiry; plan to refresh on the housekeeping thread
  regardless). Orders: add_order {order_type limit|market..., side, order_qty, symbol "BTC/USD",
  limit_price, time_in_force gtc|gtd|ioc|fok, post_only, cl_ord_id (UUID or <= 18 chars),
  validate (check without placing), deadline 500 ms to 60 s}; amend_order (keeps queue priority
  "where possible"), cancel_order, cancel_all, cancel_all_orders_after (dead man's switch:
  timeout < 86400 s, refresh every 15 to 30 s with 60 s). Executions channel: snapshot of open
  orders (snap_orders) then exec_type pending_new|new|trade|filled|canceled|expired|amended|
  restated|status, with order_id, cl_ord_id, exec_id, last_qty, last_price, cum_qty, cum_cost,
  liquidity_ind t|m, fees[{asset, qty}]; no leaves_qty (derive from order_qty - cum_qty). Book
  channel: depth 10|25|100|500|1000, snapshot then incremental updates, prices and quantities as
  JSON numbers, CRC32 checksum over the top 10 levels per message (asks low to high then bids
  high to low; per level price then qty with the decimal point and leading zeros removed,
  formatted at the pair's precision). Trade channel for prints. Instrument channel per pair:
  price_increment, qty_increment, qty_min, price_precision, qty_precision, cost_min, status
  (online, post_only, limit_only, cancel_only, maintenance...). No spot sandbox: validate=true is
  the dry run.
- Kraken rate limits, the binding constraint for a quoter: a counter per pair, max 60/125/180
  (Starter/Intermediate/Pro), decaying 1/2.34/3.75 per second. add +1, amend +1 (+3 if the order
  is under 5 s old, +2 under 10 s, +1 under 15 s), cancel by order age +8 (<5 s), +6 (<10 s), +5
  (<15 s), +4 (<45 s), +2 (<90 s), +1 (<300 s). Open orders per pair 60/80/225. Breach returns
  "EOrder:Rate limit exceeded" or "EOrder:Orders limit exceeded". A Starter account can cancel
  roughly seven fresh quotes before it must wait, so quote lifetime and amend-over-cancel are
  strategy design inputs, not tuning.
- Ontario retail constraints on Kraken: spot only (no margin, no derivatives, so no shorting:
  sells are bounded by base holdings); CSA platform orders cap non-eligible investors at $30,000
  net acquisition cost per 12 months, except the specified crypto assets BTC, ETH, BCH and LTC.
  Trade only specified assets (BTC/USD, BTC/CAD, ETH/...) against fiat; no USDT.

## What commit 2cd81d8 provides, and what it fixes in place

Present and kept:

- src/common/ipc/market_update.hpp: exec::MarketUpdate<depth> {event_seq, event_time, tsc_in,
  stock_id[8], best_bid, best_ask, trade_price, trade_size, trade_side, nb, na, bids[depth],
  asks[depth]}, uint32 prices and sizes. With EXEC_DEPTH=5 it is 192 B (3 cache lines); the
  core_to_exec ring is 8192 slots, 1.5 MiB.
- src/exec/runner.hpp: template <Strategy S> run_strategy(S&, ExecRing&, MarketView&,
  FillModel&, shutdown): pop frame, view.on_update, fills.on_update, strat.on_tick(ctx). The
  strategy is a template parameter so on_tick inlines. This is the hot loop the rest of the plan
  builds on.
- src/exec/strategy.hpp: StrategyContext {market(), inventory(), pnl(), submit(side, px, qty),
  cancel(id), is_live(id)}, concept Strategy = on_tick(StrategyContext&) noexcept, optional
  IStrategy virtual base (unused by the loop).
- src/exec/fill_model.hpp: shadow fills from observed trade flow behind an event-time latency
  gate (EXEC_LATENCY_NS). Not kept: deleted in Phase 1 (see Gaps).
- src/exec/market_view.hpp: mid, microprice, depth imbalance, EWMA realized vol, level access.
- src/exec/strategies/{logging_strategy,dummy_quoter}.hpp.
- src/core: on_execute/on_execute_with_price return OrderBook::Trade; core_main handles ITCH 'P'
  as a trade and builds the exec frame from the already-walked 15-level snapshot.
- CMake: one exec_main_<name> executable per entry in EXEC_STRATEGIES (<name>:<Type>:<header>),
  manager launches ./exec_main_$EXEC_STRATEGY (default logging) on EXEC_CORE=3.

Gaps the plan closes (each becomes a concrete item below):

- Orders never leave the process. There is no order wire type, no gateway, no exec report path.
- uint32 prices/sizes: fine for equities (4 dp fixed point), too narrow for crypto (BTC lots of
  1e-8, alt-coin sizes in the billions, per-instrument tick sizes). Widen to int64 ticks/lots.
- No instrument identity beyond stock_id[8]; multi-instrument needs an instrument index.
- core_main's (void)exec_ring->try_push(...) drops silently when exec falls behind; exec cannot
  tell. event_seq gaps are detectable but nothing counts them.
- Strategy swap requires a new binary and a full pipeline restart: the manager treats any worker
  exit as fatal, and EXEC_STRATEGY is read once at launch.
- No shm stats from exec: inventory/PnL go to stderr; the dashboard knows nothing about trading.
- No risk gate, no kill switch, no position limits.
- The shadow FillModel is dropped (Phase 1 deletes fill_model.hpp). Live and sandbox fills come
  from venue execution reports; strategy research on historic data moves to hftbacktest (see
  "Strategy research with hftbacktest").

## Answers to the design questions

How to split the processes. Keep the split you built, and extend it rather than fold the strategy
into the book builder. The exec process consuming resolved MarketUpdate frames is the right shape
for crypto specifically: Coinbase's FIX L2 feed is top-10 aggregated levels and Kraken's book
channel at depth 10 is a top-10 snapshot plus incrementals with a top-10 checksum, so a per-message
top-N frame is literally what the venue gives you. That makes the crypto gateway a drop-in
replacement for core_main as the frame producer, the exec process needs zero venue or book code, and
a strategy swap restarts only exec while the book builder (ITCH) or venue session (live) keeps
running. The price is one ring hop plus a frame copy, about 100 to 200 ns, which you have already
accepted and which is noise against venue RTT.

The resulting processes:

- Feed producer, one of: core_main (ITCH L3 book, as today) or gateway_main's MD thread (venue
  session, small L2 book, emits the same MarketUpdate frames). Both write feed_to_exec.
- exec_main: strategy host. Pops frames, runs MarketView, the order router policy (null or live
  gateway), the risk gate and the account, calls on_tick, pushes
  OrderRequest to the order ring in live mode and consumes ExecReports back. Publishes trading stats
  to shm.
- gateway_main, one per venue: MD thread (pinned) and OE thread (pinned) plus unpinned
  housekeeping. The OE thread pops OrderRequests, encodes FIX or JSON, writes the socket, reads
  execution reports, pushes ExecReports.
- dashboard: as today plus the new shm stats blocks.
- manager: creates shm, spawns workers, restarts only exec on SIGUSR1, sets the kill switch when a
  process dies.

Rules: one pinned thread per socket direction in the gateway so a market-data burst never queues
behind an order send; everything on the wire-to-decision-to-wire path is busy-polled,
allocation-free and stamped with read_tsc(). Core map for the 6C/12T box (SMT pairs (0,1),(2,3)
...): ITCH mode keeps today's assignment (exchange 2, core hot 4, exec 3, snapshotter 6). Live
mode has no exchange_main or core_main, so exec moves to 4 (a physical core of its own), gateway
MD rx takes 2, gateway OE takes 8, housekeeping and stats 0, dashboard unpinned. Cores come from
the config file so this is a one-line change per machine. Steer the NIC RX queue IRQ to a
non-sibling core on the same socket, or use SO_BUSY_POLL.

DPDK: not for v1. It removes the kernel network stack (5 to 15 us per direction) and its jitter,
but DPDK is L2 only, so it also needs a userspace TCP stack (F-Stack, mTCP, Seastar) and TLS over
it (OpenSSL custom BIO), a dedicated NIC port or bifurcated driver, and poll cores. That is more
engineering than the rest of the gateway. Against venue RTTs of 1 to 30 ms (Coinbase, Kraken)
a 10 us saving is noise, and Kraken's rate counter caps how often a faster reaction could be
acted on anyway. What moves tick-to-trade on these
venues, in order: colocate in the venue's cloud region; persistent FIX/WS sessions, never REST per
order; pre-encoded order templates; busy-poll non-blocking sockets on pinned cores (SO_BUSY_POLL,
TCP_NODELAY, TCP_QUICKACK, ethtool -C rx-usecs 0); then, only if measurement shows kernel time
matters, socket-level bypass (Onload/VMA on Solarflare or NVIDIA NICs) which accelerates ordinary
sockets without a TCP rewrite. All transport lives in one TlsSocket class so bypass later is a
one-class change.

Does in-process hot-swap add overhead? No steady-state overhead. run_strategy already tests a
flag every iteration (shutdown); the swap request is another bit in the same word, one relaxed
load of an L1-resident line with a perfectly predicted branch. The module's code is identical
whether dlopen'd or baked into exec_main_<name>: with -fvisibility=hidden -fno-plt
-fno-semantic-interposition -Wl,-Bsymbolic every call inside the module is a direct call, globals
are RIP-relative, and PIE executables are position independent anyway. The cost is a one-off
transient at swap time (dlopen on the housekeeping thread, pre-faulting the module's pages, a few
hundred cold i-cache lines, tens of microseconds on the hot thread) plus a bounded frame backlog
during the handoff. Avoid thread_local in modules (dynamic TLS adds __tls_get_addr calls).

Can a strategy be fully inlined without being known at engine build time? Not by exec_main
calling into a plugin per tick. Inlining is a compile/link-time transformation, so any dlopen
boundary is at best an indirect call and, worse, an optimization barrier (view and account state
cannot stay in registers across it). The way out is to move the compilation boundary: keep
run_strategy<S, Router> header-only, as it already is, and have the strategy module instantiate
it. The .so contains the entire hot loop (ring pop, view update, router, strategy, risk, order
ring push) compiled and LTO'd as one unit; exec_main becomes a loader plus helper threads that
calls run() once. Your current exec_main_<name> binaries are the baked form of exactly the same
template, so both can be generated from the same EXEC_STRATEGIES list and produce identical hot
loop code. The only other route is a JIT (LLVM ORC) re-specializing bitcode per strategy at run
time: same codegen, far larger project.

## Strategy research with hftbacktest

Live and sandbox trading need no fill model: both venues push every fill as an execution report
(Coinbase 35=8 with LastPx/LastQty, Kraken executions with last_price/last_qty), and
GatewayRouter applies those to the Account. A fill model is only needed to test strategies on
historic data, where a replayed feed cannot react to our orders. Rather than maintain one in C++,
research uses hftbacktest (open source, Rust core with Python/numba bindings), which already
provides what the old shadow FillModel lacked:

- Queue position: l3_fifo_queue_model() for order-level data (exact FIFO position), or
  probabilistic models (log_prob_queue_model, power_prob_queue_model...) for L2 data.
- Latency: intp_order_latency(data) replays measured per-order latencies; feed latency comes
  from the gap between exchange and local timestamps in the data.
- Fees: trading_value_fee_model(maker, taker) matches Kraken's percentage-of-value fees
  (negative values model rebates). One rate per run, so test the current tier and the next.
- Instrument rules: tick_size and lot_size.
- Data: Tardis.dev importers (paid Kraken history), or our own recordings converted to its
  event format.

What it does not cover, and how the plan handles it:

- Kraken's rate counter: no rate-limit model, so research strategies enforce it themselves
  through a Python port of the rate model (research/rate_model.py) that reads the same
  kraken.rate_* config values as exec's C++ model, with a shared test vector file so the two
  cannot drift.
- C++ strategies: hftbacktest has no C++ API. Strategies are prototyped in Python against
  hftbacktest and the ones worth keeping are ported to the C++ Strategy concept. The port can
  drift from the backtested logic, so the calibration step (below) compares live results with a
  backtest of the same session.
- ITCH: hftbacktest is not used on the NASDAQ replay. ITCH stays the latency benchmark for the
  pipeline, run with NullRouter.

Pipeline pieces that feed research:

- kraken_recorder (src/tools, unpinned, separate from gateway_main so file I/O never touches a
  trading process): connects with the same WsClient and TlsSocket, subscribes book (depth 10 or
  more), trade and, with a token, the authenticated level3 channel; writes every raw message
  with a CLOCK_REALTIME local receive timestamp to rotating compressed files. Raw capture is
  lossless, so converters can be fixed and rerun. It can run 24/7 from Phase 3 onward, long
  before live trading, to build a Kraken history at no cost.
- research/convert_kraken.py: raw captures to hftbacktest's event format (exchange and local
  timestamps, price, qty, order id for L3), validating the book checksum while replaying.
- Order latency export: in live and sandbox sessions the gateway logs, per order, the request
  time, the venue timestamp from the ack, and the local receive time (TSC converted to wall
  clock with the calibration exec already does) through a ring drained by the housekeeping
  thread. research/convert_latency.py turns this into intp_order_latency input.
- Calibration: after a minimum-size live Kraken session, replay that session's recorded market
  data in hftbacktest with a "strategy" that issues exactly our logged order actions at their
  logged times, and compare predicted against actual fills per order (filled or not, time to
  fill, price). This isolates queue and latency model error from strategy port error and puts a
  measured error bar on backtest PnL.

## Architecture decisions

- MarketUpdate stays the single market-data contract between any feed producer and exec. It is
  widened to int64 ticks and lots, gains an instrument index and flags, and keeps a template
  depth (EXEC_DEPTH raised to 10 to match Coinbase L2 and Kraken book depth 10).
- Price and quantity: int64 ticks and lots per instrument, converted at the producer using a
  shared InstrumentTable. The ITCH producer converts its uint32 4-dp prices to ticks of 0.01 (a
  divide by 100 that the compiler folds) and shares to lots of 1. Strategies quote in ticks.
- The equity L3 book in core is unchanged in layout and behavior; core_main only widens the frame
  it emits.
- Crypto L2 book lives in the gateway MD thread: TopNBook<N> (two sorted arrays of at most N
  levels, insertion by shift) for both venues' top-N incremental feeds (Coinbase FIX L2, Kraken
  book depth 10). The radix-bitmap L2Book over PriceLevelArray is only needed if a full-depth L2
  feed is added later.
- Strategy dispatch: the existing concept plus template run loop; zero virtuals on the hot path.
  Optional hooks (on_exec, on_start, on_stop, on_timer) are detected with requires-expressions so
  on_tick remains the only mandatory method and both existing strategies compile unchanged.
- Order routing is a template policy of run_strategy: NullRouter (ITCH mode; every accepted
  order is acked and never fills, so tick-to-order latency, the OrderTable and the risk gate are
  exercised without a venue) and GatewayRouter (sandbox and live; pushes OrderRequests, consumes
  ExecReports into the Account). There is no in-process fill model; strategy research on
  historic data uses hftbacktest.
- Strategy loading: v1 keeps exec_main_<name> baked binaries and adds a generic exec_main that
  dlopens strategy_<name>.so chosen at start; swap by the manager restarting only exec. v1.5:
  in-process hot-swap on SIGHUP with state handoff, designed in now.
- Gateway polymorphism: template <class Venue> run_gateway() plus a switch on venue id at
  startup. No virtuals or RTTI, matching -fno-rtti.
- Dependencies: system OpenSSL 3 (TLS, HMAC-SHA256 for Coinbase, SHA-256 and HMAC-SHA512 for
  Kraken, SHA-1 for the WS handshake); simdjson via FetchContent with SIMDJSON_EXCEPTIONS=OFF;
  hand-written WebSocket client, minimal HTTPS/1.1 client (Kraken token, Coinbase /products) and
  FIX codec/session; a table-driven CRC32 (~30 lines) for Kraken's book checksum. No Boost, no
  DPDK. CMake option LL_BUILD_LIVE keeps the
  ITCH-only build free of OpenSSL; LL_BUILD_DASHBOARD makes glfw3/OpenGL optional.
- Secrets: API key, secret and passphrase (Coinbase sandbox) or API key and secret (Kraken) come
  from env vars or a 0600 file named in config, never argv (visible in ps). The Kraken key gets
  query and trade permissions only, never withdraw.
- Safety: pre-trade risk gate in exec; kill-switch bit in shm settable by dashboard, manager or
  exec death; gateway cancels all on kill or stale exec heartbeat; venue-side backstops are
  Coinbase 8013=Y (cancel on disconnect) and Kraken cancel_all_orders_after (60 s timer refreshed
  every 15 s by the OE thread, so a dead gateway cancels within a minute); TLS certificate
  verification always on. Real money only ever goes to Kraken, only with live=1 in config, and
  starts in PAUSE until the dashboard releases it.

## Wire contracts

All POD, static_assert trivially copyable and exact sizeof, 64-byte aligned.

src/common/ipc/market_update.hpp (v2 of the existing frame)

    namespace exec {
    struct Level { int64_t price; int64_t qty; uint32_t order_count; uint32_t _pad; };   // 24 B
    enum FrameFlags : uint8_t { TWO_SIDED = 1, SNAPSHOT = 2 /*full book rebuild*/, FEED_RESET = 4, LAST_IN_BATCH = 8 };
    template <std::size_t depth> struct alignas(64) MarketUpdate {
      uint64_t event_seq;      // producer-monotonic; a gap means frames were dropped
      uint64_t event_time;     // ITCH ns since midnight or venue ns since epoch
      uint64_t tsc_in;         // producer read_tsc() at ingest (parser or socket read)
      int64_t  best_bid, best_ask;          // ticks; 0 / INT64_MAX when a side is empty
      int64_t  trade_price, trade_qty;      // trade carried by this frame; trade_qty==0 means none
      uint16_t instrument;     // index into ref::InstrumentTable
      common::Side trade_side; uint8_t flags;
      int16_t  nb, na; uint16_t _pad;
      Level bids[depth], asks[depth];
    };
    inline constexpr std::size_t EXEC_DEPTH = 10;
    using FeedRing = common::SpscRing<MarketUpdate<EXEC_DEPTH>, 8192>;   // frame 576 B, ring 4.5 MiB
    }

The frame grows from 192 B to 576 B (9 lines). If the ITCH benchmark shows the extra copy, keep
EXEC_DEPTH at 5 for ITCH (320 B) and make depth a config-selected instantiation; the loop is
templated on it already.

src/common/ipc/order_msgs.hpp

    namespace oe {
    enum class ReqType : uint8_t { New=1, Cancel=2, Replace=3, CancelAll=4 };
    enum class OrdType : uint8_t { Limit=1, Market=2 };
    enum class Tif     : uint8_t { GTC=1, IOC=2, FOK=3, PostOnly=4 };   // PostOnly = Coinbase ExecInst=A / Kraken post_only
    struct alignas(64) OrderRequest {              // 64 B
      uint64_t cl_ord_id;      // (exec_epoch << 40) | counter, unique across exec restarts
      uint64_t orig_cl_ord_id; // Cancel/Replace target
      uint64_t tsc_decision;   // read_tsc() at submit
      uint64_t strategy_tag;   // opaque, echoed in ExecReport
      int64_t  price; int64_t qty;   // ticks / lots
      uint16_t instrument; ReqType type; common::Side side; OrdType ord_type; Tif tif; uint8_t flags; uint8_t _pad;
    };
    enum class ExecType  : uint8_t { Ack=1, Reject=2, PartialFill=3, Fill=4, Canceled=5, CancelReject=6, Replaced=7, Expired=8, Restated=9 };
    enum class OrdStatus : uint8_t { PendingNew=0, New, PartiallyFilled, Filled, Canceled, Rejected, Expired };
    struct alignas(64) ExecReport {                // 128 B, the venue id string forces two lines
      uint64_t cl_ord_id, orig_cl_ord_id, strategy_tag;
      uint64_t tsc_sent;       // from the gateway's order table, 0 for unsolicited
      uint64_t tsc_recv;       // gateway read_tsc() on receipt
      uint64_t venue_ts_ns;
      int64_t  last_px, last_qty, cum_qty, leaves_qty, fee;   // fee in 1e-8 units of fee_ccy
      uint16_t instrument; ExecType exec_type; OrdStatus ord_status;
      uint8_t  liquidity /*0 unk,1 maker,2 taker*/, reject_reason, fee_ccy /*0 quote,1 base*/, _pad;
      char     venue_order_id[32];   // UUID text (Coinbase) or order_id like OUF4EM-FRGI2-MQMWZD (Kraken)
    };
    using OrderRing = common::SpscRing<OrderRequest, 4096>;  // 256 KiB
    using ExecRing  = common::SpscRing<ExecReport, 4096>;    // 512 KiB (rename today's exec::ExecRing to FeedRing first)
    }

src/common/ipc/instrument.hpp

    namespace ref {
    struct alignas(64) Instrument {                // 64 B; int64s first so it packs
      int64_t tick_mant, lot_mant;          // tick size = tick_mant * 10^tick_exp, lot likewise
      int64_t min_qty_lots;                 // Coinbase base_min_size / Kraken qty_min
      int64_t min_notional;                 // quote 1e-8 units (Kraken cost_min)
      char    symbol[16];                   // venue native: "BTC-USD", "BTC/USD", or the ITCH ticker
      char    venue[8];
      int8_t  tick_exp, lot_exp;
      uint8_t price_precision, qty_precision;   // decimal places for wire strings and Kraken's checksum
      uint8_t status;                       // online, post_only, limit_only, cancel_only, halted
      uint8_t _pad[3];
    };
    struct InstrumentTable { std::atomic<uint32_t> count; uint32_t _pad; Instrument rows[256]; };  // 16 KiB
    }

src/common/ipc/stats.hpp, seqlock payloads written single-writer by the owning thread and read
by dashboard and manager; histograms reuse common::Histogram (528 B each)

    namespace stats {
    struct ExecStats    { common::Histogram feed_transit /*pop - tsc_in*/, tick /*on_tick duration*/, tick_to_order /*push - tsc_in*/, exec_transit;
                          uint64_t frames, frame_gaps, orders_new, orders_cancel, risk_rejects, ring_full_rejects,
                                   iterations, heartbeat_tsc, exec_generation; uint32_t strategy_id; char strategy_name[32]; };
    struct FeedStats    { uint64_t frames_pushed, frames_dropped, events_in, book_resets, heartbeat_tsc; uint8_t mode; };   // core_main or gateway MD
    struct GatewayStats { common::Histogram parse, order_encode_send, ack_rtt, fill_rtt, cancel_rtt;
                          uint64_t md_msgs, md_bytes, oe_msgs_in, oe_msgs_out, seq_gaps, resyncs, reconnects,
                                   last_md_tsc, last_oe_tsc, orders_sent, acks, fills, cancels, rejects, cancel_rejects,
                                   rate_budget_remaining, heartbeat_tsc; uint8_t md_state, oe_state; char venue[8]; };
    struct Position     { int64_t qty_lots, avg_px_ticks, realized_pnl, unrealized_pnl, fees, bought, sold, peak_equity, max_drawdown;
                          uint32_t open_orders, fills, orders, cancels, rejects; uint16_t instrument; };
    struct PositionStats { Position rows[16]; uint32_t count; };
    struct OpenOrder    { uint64_t cl_ord_id, tsc_sent; int64_t px, qty, leaves; uint16_t instrument; common::Side side; oe::OrdStatus st; };
    struct OpenOrders   { OpenOrder rows[64]; uint32_t count; };
    }

src/common/ipc/control.hpp

    namespace ipc {
    enum ControlBits : uint32_t { SHUTDOWN=1, KILL=2 /*cancel all, no new*/, PAUSE=4 /*no new*/, SWAP=8 /*exec reloads module*/ };
    struct alignas(64) Control { std::atomic<uint32_t> flags; uint32_t _pad; char swap_path[256-8]; };
    }

src/common/ipc/shm.hpp v3. Bump ipc::VERSION to 3 (2 is taken by the current commit). ShmHeader
gains layout_hash, a constexpr FNV over sizeof/offsetof of every wire struct. Every worker checks
both and aborts with a message.

    struct alignas(64) PipelineShm {
      ShmHeader header; ipc::Control control; ref::InstrumentTable instruments;
      core::CoreRing exchange_to_core;            // ITCH, unchanged
      exec::FeedRing feed_to_exec;                // was core_to_exec; producer is core_main or the gateway MD thread
      oe::OrderRing exec_to_gateway; oe::ExecRing gateway_to_exec;
      dashboard::DashboardRing core_to_dashboard;
      common::Seqlock<stats::FeedStats> feed_stats; common::Seqlock<stats::ExecStats> exec_stats;
      common::Seqlock<stats::GatewayStats> gateway_stats;
      common::Seqlock<stats::PositionStats> positions; common::Seqlock<stats::OpenOrders> open_orders;
    };  // about 6.5 MiB of the 16 MiB segment

dashboard::Snapshot becomes SnapshotV2 with int64 price/qty levels, an instrument field and
price_exp so the GUI formats crypto and equity prices identically. In live mode the dashboard
frame is produced by the gateway MD thread's snapshotter from its TopNBook (same
run_snapshotter, same DashboardPublisher), so the book panel works without core_main.

Infrastructure touch-ups in src/common that ride along:

- SpscRing::try_claim() and publish(), a two-phase producer API so producers stamp tsc_in as the
  last store before the release. This formalizes what ItchParser::next() does by poking slots[]
  directly (src/exchange/itch/itch_parser.cpp lines 40-48 and 186-187). Fix its back-pressure
  bound (EXEC-era EXCHANGE_RING_CAPACITY 4096 against the 8192 ring) and the uint64_t tail widening.
- Ring-full policy: feed ring full means the producer drops, counts in FeedStats and sets FEED_RESET
  on the next frame it does push, never blocking; exec detects the gap from event_seq, counts
  frame_gaps and passes the flag to the strategy through MarketView. Exec ring
  (reports) full means the gateway spins with backoff, fills are never dropped. Order ring full
  means exec counts a ring_full_reject and the strategy sees the submit fail.
- ShmSegment::create() falls back from MFD_HUGETLB to normal pages with a warning so unit and
  loopback tests run on hosts without reserved hugepages.

## Exec process changes (src/exec)

Keep the files and the shape; extend them.

- strategy.hpp: StrategyContext becomes a template on the Router and exposes, in addition to the
  current methods, instrument(i), now_tsc(), position(i) (lots, avg px, realized, unrealized),
  open_orders(i), submit(side, px, qty, Tif = GTC, OrdType = Limit), replace(id, px, qty),
  cancel_all(). submit assigns cl_ord_id, stamps tsc_decision, runs the RiskGate, records in the
  OrderTable and hands the request to the Router. The existing concept stays; optional hooks
  on_start(ctx), on_stop(ctx), on_exec(ctx, const ExecReport&), on_timer(ctx, tsc) are called
  only if present (requires-detection in runner.hpp). IStrategy is removed or left unused.
- router.hpp: NullRouter {submit acks immediately, cancel cancels, nothing fills} and
  GatewayRouter {OrderRing* out; ExecRing* in; OrderStateView; on_frame(u) is a no-op; poll()
  drains ExecReports into OrderTable and Account and dispatches on_exec}. Both satisfy a
  Router concept.
- account.hpp: per-instrument qty, average price, realized and unrealized at mid, fees, bought,
  sold, peak equity, max drawdown, in int64 ticks and lots (__int128 for the products), plus
  per-asset balances (base and quote holdings, available versus reserved by open orders). Live
  mode seeds balances from the venue at start (Kraken balances via the private WS or REST
  Balance; Coinbase sandbox via REST /accounts) and the gateway reports them as Restated rows;
  null mode takes them from config.
- order_table.hpp: fixed 4096 slots, open-addressed by cl_ord_id (same shape as core's
  OrderMap); live orders, states, leaves; snapshot into stats::OpenOrders for the dashboard.
- risk.hpp: max |position| lots and notional, max order qty and notional, max open orders, price
  band versus mid, KILL and PAUSE bits read from ipc::Control. Spot-only for the live venue: a
  sell may not exceed available base and a buy may not exceed available quote, counting open
  orders as reserved (no shorting on Kraken for Ontario retail). A venue rate model replaces the
  generic orders-per-second bucket: a per-instrument counter mirroring Kraken's (configured
  max and decay for the account tier, +1 per add or amend, age-dependent cancel and amend
  penalties from the OrderTable's ack time, with a safety margin) plus Kraken's per-pair open
  order limit; Coinbase uses a plain token bucket. An instrument allowlist (config) restricts
  live trading to the specified assets (BTC, ETH, BCH, LTC) so the $30,000 net-acquisition cap
  never applies, and orders respect the instrument's min qty, min notional and status.
  Rejections are counted per reason and returned to the strategy as INVALID_ORDER.
- fill_model.hpp: deleted in Phase 1 along with EXEC_LATENCY_NS.
- runner.hpp: run_strategy<S, Router>(S&, FeedRing&, Router&, ExecContext&) returns RunExit
  {Shutdown, Swap}. Loop body: one relaxed load of control.flags; pop frame (count gaps via
  event_seq); view.on_update; router.on_frame; router.poll(); strat.on_tick(ctx); every
  TIMER_MASK iterations on_timer if present, publish ExecStats/PositionStats/OpenOrders to shm,
  update heartbeat_tsc. Timestamps: feed_transit = t_pop - tsc_in, tick = on_tick duration,
  tick_to_order = t_push - tsc_in for frames that produced an order.
- exec_main.cpp: args --mode=null|live --strategy=<builtin|path.so> --config=<file>; attaches
  shm (version and layout check), pins to the configured core, builds ExecContext in static
  storage (Account, OrderTable, RiskLimits, instrument table pointer, rings), loads the strategy
  (baked type or module), runs the loop handling RunExit::Swap, and on shutdown calls on_stop,
  cancel_all in live mode, drains reports for up to 2 s, and writes strategy_stats.log.
- module_abi.hpp: the only C ABI between exec_main and a module:

    extern "C" struct ll_strategy_module_v1 {
      uint32_t abi_version /*1*/, layout_hash;
      const char *name, *build_id;
      void*  (*create)(exec::ExecContext*, const char* params);
      exec::RunExit (*run_null)(void*, exec::ExecContext*);
      exec::RunExit (*run_live)(void*, exec::ExecContext*);
      size_t (*export_state)(void*, void* buf, size_t cap);   // strategy-private POD with a versioned header
      bool   (*import_state)(void*, const void*, size_t);
      void   (*destroy)(void*);
    };
    extern "C" const ll_strategy_module_v1* ll_strategy_module();   // sole exported symbol
    #define LL_STRATEGY_MODULE(Type)   // instantiates run_strategy<Type, NullRouter> and run_strategy<Type, GatewayRouter>

- loader.hpp/.cpp: copies the .so into a memfd and dlopens /proc/self/fd/N with
  RTLD_NOW|RTLD_LOCAL (defeats dlopen's inode cache when rebuilding in place and avoids mapping a
  file being rewritten on NFS), dlsym("ll_strategy_module"), checks abi_version, layout_hash,
  build_id and __builtin_cpu_supports for the ISA the module was built with (-march=native modules
  are machine-specific; fail loudly instead of SIGILL), pre-faults text and data (dl_iterate_phdr
  plus mlock, best effort under RLIMIT_MEMLOCK). builtin:<name> resolves through the same table
  built from in-executable instantiations, so exec_main_<name> binaries and exec_main + .so share
  one code path.
- scripts/check_strategy_so.sh runs as a post-build step on every module and fails if the module
  has a PT_TLS segment, a non-trivial .fini_array, undefined non-libc symbols, or exports
  anything beyond ll_strategy_module (linker version script src/exec/strategies/export.map with
  { global: ll_strategy_module; local: *; };).
- Hot-swap protocol (v1.5, SIGHUP or Control.SWAP plus swap_path): the housekeeping thread
  dlopens the new module and pre-faults it, publishes a pending pointer, sets the SWAP bit. The
  hot thread returns from run_strategy at the end of the current frame, then the hot thread
  itself does old.export_state, new.create, new.import_state, old.destroy, run again. Account,
  OrderTable and MarketView live in ExecContext and are untouched. Whether resting orders are
  cancelled on swap is a config knob (swap.cancel_open=1 default). The old .so stays mapped by
  default (dlclose optional via LL_DLCLOSE_OLD=1). Modules: no thread_local, no static
  destructors, no exceptions, enforced by check_strategy_so.sh.
- Strategies: keep logging and dummy (DummyQuoter's stderr logging moves behind a verbosity flag;
  its PnL now comes from Account). Add imbalance_taker (IOC when depth imbalance crosses a
  threshold; exercises the taker path).
  StrategyContext exposes rate_budget(i) so quoters can plan around Kraken's counter: prefer replace
  (Kraken amend_order, which keeps queue priority where possible and costs less than cancel plus
  add), hold quotes for a minimum lifetime, and widen rather than requote when the budget is low.
  The rate model runs in every mode, so ITCH runs exercise it too.

## Core (ITCH feed producer) changes

- make_market_update: convert uint32 4-dp prices to ticks (divide by PRICE_TICK), shares to
  lots, fill instrument (0 for the single ITCH symbol, registered in the InstrumentTable by
  core_main at start with tick 0.01 and lot 1), set TWO_SIDED and LAST_IN_BATCH (always, one
  event per frame), count drops in FeedStats and set FEED_RESET on the frame after a drop.
- Publish order: push the exec frame before the dashboard seqlock store so strategy latency is
  not behind the 448 B dashboard copy.
- 'Q' cross trades: treat like 'P' (trade with no book effect) so auction prints reach exec.
- Optional, benchmark-gated: skip the frame when nothing in the top EXEC_DEPTH levels or the
  trade changed (cuts exec ring traffic from deep-book churn; the strategy loses per-event tick
  rate, which the view can keep as a counter in the frame instead).

## Gateway process (src/gateway)

gateway_main --md=<venue|none> --oe=<venue|none> --config=<file>. A switch instantiates
run_gateway<CoinbaseFix>() (test venue) or run_gateway<KrakenWs>() (live venue).

MD thread: owns the venue MD session; parses; applies to a TopNBook<EXEC_DEPTH> per instrument
(Coinbase and Kraken are both snapshot-plus-incremental top-N feeds); after each venue message
emits one MarketUpdate with the last trade if the message carried one, tsc_in = read_tsc() after
the socket read that completed the message, event_time = venue timestamp; SNAPSHOT on the first
frame after a snapshot (Coinbase W, Kraken book snapshot) or a resync, FEED_RESET after a gap
or a Kraken checksum mismatch; registers instruments in the InstrumentTable
at startup; runs the dashboard snapshotter for the display instrument; publishes FeedStats and
GatewayStats MD fields.

OE thread: pops exec_to_gateway, encodes and writes (stamps tsc_sent into the OrderStateTable),
polls the OE socket, pushes ExecReports with tsc_recv, sends heartbeats (and on Kraken refreshes
cancel_all_orders_after), watches Control.KILL and the exec heartbeat_tsc (stale beyond 500 ms
triggers cancel_all()).

Housekeeping (unpinned): logging drain, instrument refresh, reconnect backoff coordination,
Kraken WS token fetch and refresh over HTTPS (handed to the OE thread through a seqlock, never
fetched on the hot path), config reload on SIGHUP.

Exec restart handoff: exec writes exec_generation into ExecStats at start. When the gateway sees
it change it replays every live row of its OrderStateTable as ExecType::Restated reports so the
new exec adopts the open orders, or cancels them if the new strategy does not recognise the
strategy_tag. This makes "restart exec to switch strategies" safe with resting quotes.

Transport in src/gateway/net, all noexcept, fixed buffers, no allocation after connect:

- tcp.hpp: non-blocking connect, TCP_NODELAY, TCP_QUICKACK, SO_BUSY_POLL, buffer sizes.
- tls_socket.hpp/.cpp: OpenSSL 3 SSL on the fd, TLS 1.3, certificate verification against the
  system bundle (never disabled), SSL_MODE_ENABLE_PARTIAL_WRITE and ACCEPT_MOVING_WRITE_BUFFER,
  read()/write() returning WANT_* as codes, a 1 MiB receive ring per socket. This is the single
  seam for future kernel bypass.
- ws_client.hpp/.cpp: RFC 6455. HTTP/1.1 Upgrade with a random Sec-WebSocket-Key and verification
  of Accept = base64(SHA1(key+GUID)), frame parser (FIN, opcode, 7/16/64-bit lengths,
  continuation), client-side masking with a 64-bit XOR loop, ping to pong, close handshake, no
  permessage-deflate.
- fix/codec.hpp/.cpp: fix::Encoder (4 KiB buffer, tag=value SOH, per-message-type pre-built
  header template with slots for 34 and 52, computes 9 BodyLength and 10 CheckSum on finish) and
  fix::Decoder (SOH tokenizer, checksum verify, tag to view lookup via a sparse array for tags
  below 10000, repeating-group cursor for 268/NoMDEntries). SendingTime YYYYMMDD-HH:MM:SS.sss
  from clock_gettime(CLOCK_REALTIME).
- fix/session.hpp/.cpp: template <class Transport> class FixSession, a FIXT.1.1 state machine:
  Logon (A) with the HMAC-SHA256 signature, Heartbeat (0) and TestRequest (1) with TSC deadlines,
  Logout (5), Reject (3), ResendRequest (2) answered with SequenceReset-GapFill (4, 123=Y), inbound
  sequence gap to ResendRequest (at most 1000), reconnect with backoff, reset_seq_on_logon for the
  sandbox. Transport is TlsSocket in production, MemoryTransport (scripted peer, injected clock)
  in tests, RecordingTransport (wraps TlsSocket, writes SOH-preserving captures for replay tests).
- https_client.hpp/.cpp: blocking HTTP/1.1 over TlsSocket for the housekeeping thread only
  (Coinbase REST /products and /accounts, Kraken GetWebSocketsToken and Balance); Content-Length
  and chunked bodies, no redirects, keep-alive.
- json: simdjson On-Demand wrappers (error-code API) and a hand-written JsonWriter over a fixed
  buffer. Kraken sends prices and quantities as JSON numbers, so they are read as raw number
  tokens and converted to ticks and lots by exact decimal parsing, never through double.
- decimal.hpp: ticks and lots to and from decimal strings at the instrument's precision; the
  same formatter produces Kraken checksum strings and FIX 44/38 fields.
- crc32.hpp: table-driven CRC32 (IEEE, as zlib) for Kraken's book checksum.
- book/topn_book.hpp: TopNBook<N, Traits>: two fixed arrays sorted best-first, set_level(side,
  px, qty) inserts, updates or deletes by shift (N <= 32 so this is a few cache lines), reset(),
  fill_frame(MarketUpdate&). Unit-tested against a std::map oracle.

Venue adapters in src/gateway/venues:

- coinbase_fix.hpp/.cpp. Market data: logon to fix-md:6121, 35=V with 264=10 (L2) and 267/269 =
  0,1,2. W rebuilds the TopNBook and emits a SNAPSHOT frame; X applies each entry (279 new/change/
  delete, 269 bid/offer/trade) and emits one frame per message with the trade if present. An
  RptSeq gap or session drop triggers re-request and a FEED_RESET frame. Order entry: logon to
  fix-ord:6121 with 8013=Y. OrderRequest becomes 35=D (11 = reversible UUID text encoding of
  cl_ord_id plus session nonce with version and variant bits fixed; 55; 54; 40=2 or 1; 59=1, 3 or
  4; 18=A for PostOnly; 44 and 38 as decimals from ticks and lots), 35=F (37 from OrderStateTable,
  41), 35=G. 35=8 becomes ExecReport from tags 150, 39, 31, 32, 14, 151, 1057, 12, 60; 35=9 becomes
  CancelReject. Instrument table from REST /products once at startup. cancel_all() sends 35=F per
  open order since Coinbase FIX has no mass cancel; 8013 covers disconnects. L3 (264=0) is a later
  option that would reuse the core L3 book with UUIDs hashed to 64-bit refs.
- kraken_ws.hpp/.cpp (Phase 5, live venue). Two WSS connections, matching the one thread per
  socket rule: public ws.kraken.com/v2 on the MD thread, private ws-auth.kraken.com/v2 on the OE
  thread. Market data: subscribe instrument (fills the InstrumentTable), book depth 10 and trade
  for the configured symbols. The book snapshot rebuilds the TopNBook and emits a SNAPSHOT frame;
  each update applies its levels (qty 0 deletes), verifies the CRC32 against the top 10, and emits
  one frame; a trade message emits a frame with the trade fields. Checksum mismatch or a
  reconnect resubscribes the book and emits FEED_RESET. Order entry: subscribe executions with
  snap_orders=true (reconciles the OrderStateTable on every connect) and balances. OrderRequest
  becomes add_order (cl_ord_id = the same reversible UUID encoding as Coinbase; post_only for
  PostOnly; limit_price and order_qty from ticks and lots at the pair's precision; deadline set
  so a stale order is rejected by the engine rather than placed late), Replace becomes
  amend_order, Cancel becomes cancel_order, CancelAll becomes cancel_all (one message, unlike
  Coinbase). Executions map to ExecReport: exec_type new to Ack, trade to PartialFill or Fill by
  cum_qty, canceled, expired, amended to Replaced, restated; leaves = order_qty - cum_qty;
  liquidity_ind t|m; fees[0] to fee and fee_ccy. Request errors (success=false with req_id) map
  to Reject or CancelReject with the venue error text mapped to reject_reason (rate limit, orders
  limit, insufficient funds, post-only would cross, other). The ratecounter option on
  executions feeds GatewayStats.rate_budget_remaining and corrects exec's rate model.
  cancel_all_orders_after(60) is armed at logon, refreshed every 15 s, and disarmed on clean
  shutdown after cancel_all. Every order path is first run with validate=true.

## Manager and configuration

- manager <pipeline.cfg> replaces the inline workers[] array and the EXEC_STRATEGY env var. A
  small key=value parser (src/common/config_file.hpp, no exceptions; .cfg extension since *.txt
  is gitignored). Keys: mode (itch|live), md_venue, oe_venue, strategy (builtin:<name> or path to
  .so), exec.mode (null|live), params, feed_path, symbols, cores.*, risk.*, swap.cancel_open,
  coinbase.key_env/secret_env/passphrase_env/sender_comp_id/target_comp_id/host/port (sandbox
  hosts only), kraken.key_env/secret_env/symbols/rate_max/rate_decay/max_open_per_pair (rate
  values from the account tier), live (must be 1 for any order to reach Kraken),
  risk.allowed_assets (default BTC,ETH). Workers receive the shm fd plus --config=<path>.
- SIGUSR1 restarts only exec with the strategy named in Control.swap_path if set, else the config
  value: strategy iteration without touching the book builder or the venue session. Exec death
  sets Control.KILL (gateway cancels all) then respawns exec if auto_restart_exec=1. Gateway or
  core death sets KILL and exec PAUSE. Dashboard death is ignored. Any other unexpected exit stays
  fatal as today.
- ShmHeader.version and layout_hash are checked by every worker; mismatch aborts with a message.

## Dashboard metrics, prioritized

Read from shm: the SnapshotV2 ring (book, as now), feed_stats, exec_stats, gateway_stats,
positions, open_orders, control (a kill button writes KILL). New panels: a status strip, a PnL and
position table, an execution panel, and latency panels replacing the single p99/p999 line.

Latency, internal:
- tick-to-order p50/p99/p99.9/max: order push tsc minus frame tsc_in. The number this project
  exists to minimize.
- decomposition: gateway parse, feed transit (ring hop), on_tick duration, order ring transit,
  encode and send. One histogram each; shows where microseconds go when something regresses.
- latency heatmap (time by log2 bucket): tails over time; per-frame deltas of the existing
  64-bucket Histogram feed it directly.

Latency, venue:
- ack RTT, fill RTT, cancel RTT p50/p99: tsc_recv minus tsc_sent. Separates venue and network
  from us; alerts on venue degradation.
- feed staleness (now minus last_md_tsc) and venue timestamp versus local wall clock skew. A
  stale feed means quoting blind; drives auto-pause.

Execution quality:
- fill ratio, cancel ratio, order-to-trade ratio, reject count by reason: strategy health and
  venue rate-limit hygiene.
- maker/taker fill split and fees paid: the fee tier dominates PnL for quoters.
- markouts: fill price versus mid at +100 ms, +1 s, +5 s signed by side. The adverse-selection
  metric; computed in exec from a small mid history ring.
- spread capture per maker fill and effective spread for takers: shows whether quotes earn the
  spread or get picked off.

Risk and position:
- position in lots and notional per instrument, net and gross exposure, with limit utilization
  bars from RiskLimits; balances per asset, available versus reserved by open orders (spot only,
  so available base bounds how much can be offered).
- realized, unrealized and total PnL curve, fees, peak-to-trough drawdown, marked at mid.
  Drawdown drives the kill decision.
- open orders table (side, price, leaves, age) and oldest unacked order age. Stuck orders mean
  session trouble.
- kill-switch state, PAUSE state, risk rejects per second. Must be visible at a glance.

Session health:
- MD and OE state (connecting, logon, active), reconnects, sequence gaps or checksum failures,
  resyncs, heartbeat age, rate budget remaining (Kraken: per-pair counter against its max, the
  constraint a quoter hits first), dead man's switch armed and time to trigger, WS token age. One
  strip per venue session.

Pipeline health:
- ring occupancy and high-water marks for the feed, order and report rings; frames dropped by
  the producer and gaps seen by exec. Replaces the current single queue-depth plot, which caps at
  50% today (see Risks).
- exec iterations per second versus frames per second, TSC calibration drift. Detects a
  de-pinned or throttled hot thread.

Market (keep): bid/ask/microprice, microtrend, imbalance, tick rate, depth table, now for the
active instrument.

## CMake layout

- option(LL_BUILD_LIVE "Build gateway/live venues (needs OpenSSL)" ON) with find_package(OpenSSL)
  only under it. FetchContent simdjson (SIMDJSON_EXCEPTIONS OFF). LL_BUILD_DASHBOARD makes the
  glfw3/OpenGL lookups conditional so headless boxes can build the engine.
- kraken_recorder is built under LL_BUILD_LIVE. research/ is Python (hftbacktest, numpy,
  numba; pinned in research/requirements.txt) and outside CMake; its tests (converter round
  trips, rate model against the shared vectors) run with pytest.
- add_library(ll_common INTERFACE) for src/common; add_library(ll_exec INTERFACE) for src/exec
  headers; add_library(ll_gateway_net STATIC ...).
- The existing EXEC_STRATEGIES list drives two outputs per entry: the baked exec_main_<name>
  (as today) and a MODULE library strategy_<name> built from a one-line TU
  (#include header; LL_STRATEGY_MODULE(Type)) with the project flags plus -fPIC
  -fvisibility=hidden -fvisibility-inlines-hidden -Wl,-Bsymbolic -Wl,-z,now, the version script,
  LTO, and the check_strategy_so.sh post-build step. Plus a generic exec_main that links dl.
- Executables: manager, exchange_main, core_main, exec_main, exec_main_<name>..., gateway_main,
  dashboard.
- Tests, hand-rolled CHECK style with a shared tests/check.hpp extracted from
  tests/test_orderbook.cpp lines 14-22, wired into ctest via enable_testing(): test_wire_layout
  (sizeof/offsetof goldens, layout_hash stability), test_topn_book (randomized ops against a
  std::map oracle like test_level_bitmap.cpp), test_decimal (ticks and lots to and from decimal
  strings at a given precision, raw JSON number tokens to ticks with no double round trip),
  test_account, test_order_table, test_risk_gate,
  test_rate_model (Kraken counter: add, amend and cancel-by-age costs, decay, open order limit),
  test_kv_config, test_fix_codec (checksum and BodyLength vectors, repeating groups, split and
  merged TCP segments, UUID to cl_ord_id), test_fix_session (MemoryTransport: logon HMAC known
  answer, heartbeat and test-request timers with an injected clock, gap to resend to gap-fill,
  logout), test_ws_frame (RFC 6455 vectors including the dGhlIHNhbXBsZSBub25jZQ== accept key),
  test_module_abi (dlopen strategy_logging and strategy_dummy, layout hash, dummy to logging to
  dummy swap preserving Account and OrderTable; a fixture module with thread_local must be
  rejected), test_kraken_sign (API-Sign known-answer vector from Kraken's REST authentication
  docs), test_kraken_checksum (the book checksum example from Kraken's guide, then recorded
  snapshots and updates), test_kraken_messages (recorded executions, book, trade and error
  responses mapped to ExecReport and frames).

## Phasing

Each phase builds and passes its tests alone.

1. Contracts and shm v3 (small): widen MarketUpdate to int64 ticks/lots with instrument and flags
   (and the mechanical int64 changes in MarketView, StrategyContext and core_main's
   make_market_update), delete the shadow FillModel, rename core_to_exec to feed_to_exec, add order
   and report rings, InstrumentTable, Control, stats blocks, SnapshotV2, version 3 plus layout_hash
   checks in all workers, try_claim/publish, hugepage fallback, ITCH parser capacity fix, FeedStats
   drop counting and event_seq gap detection in exec. The dashboard still renders; exec_main_dummy
   runs as today.
2. Exec as a trading engine on the ITCH path (large): Router policy (NullRouter), Account,
   OrderTable, RiskGate, extended StrategyContext and optional hooks, run_strategy returning
   RunExit, ExecStats/PositionStats/OpenOrders published to shm, exec_main args and config, module
   ABI plus loader plus builtin registry, strategy_<name>.so targets alongside exec_main_<name>,
   imbalance_taker, dashboard PnL, position, open-orders and execution panels, manager config file
   and SIGUSR1 exec restart with death rules. Deliverable: strategies running on ITCH replay with
   orders, risk and tick-to-order on the dashboard; swap strategies by restarting exec without
   touching core_main.
3. Transport and FIX (medium): TlsSocket, WsClient, HttpsClient, decimal, fix::Encoder, Decoder
   and Session with tests and a recorded-message replay harness. Builds only under LL_BUILD_LIVE.
4. Coinbase Exchange sandbox, the test venue (large): TopNBook, gateway_main skeleton with MD
   and OE threads, GatewayRouter in exec, coinbase_fix MD (L2) and OE, instrument table,
   reconnect and resync, exec restart handoff via Restated reports, gateway-side dashboard
   snapshotter. Run --md=coinbase_fix with exec in null mode first (sandbox data, no orders
   sent), then live OE in the sandbox. Venue RTT and session panels. Every gateway, router, risk
   and handoff behavior is proven here first, since the sandbox is the only venue where mistakes
   cost nothing.
5. Kraken live (large): kraken_ws MD (instrument, book with CRC32, trade) and OE (token,
   executions, balances, add/amend/cancel/cancel_all, dead man's switch), rate model in the risk
   gate, spot balance checks, asset allowlist, live=1 and PAUSE-by-default. Staged: (a) MD only
   with exec in null mode on live Kraken data; (b) OE with validate=true for every order type
   and error path; (c) live post-only orders at the minimum size far from touch, then cancel;
   (d) a quoting strategy at minimum size with tight risk limits.
6. Hot-swap and hardening (medium): SIGHUP in-process swap with state handoff, chaos tests (kill
   exec mid-session so the gateway cancels all; drop the socket and resync; run against the Coinbase
   sandbox, then repeat the kill drills once on Kraken at minimum size), socket tuning, README and
   benchmarks.md update with a tick-to-order section while keeping ITCH numbers comparable.
Research track (parallel from Phase 3, medium): kraken_recorder, research/ with hftbacktest,
   Kraken data conversion, fee- and rate-aware backtests, level3 and measured latency, and
   calibration against live minimum-size sessions. See "Strategy research with hftbacktest".
7. Benchmark-gated optimizations (small to medium), each with a before/after benchmarks.md
   entry: repack common::Event to 64 B (today 128 B, two lines per pop), a producer-side cached
   head in SpscRing, EXEC_DEPTH 5 instantiation for ITCH if the 576 B frame shows, skip unchanged
   frames, capture the dashboard frame on demand instead of per event, optional log-linear
   Histogram sub-buckets.

## Verification

- Unit tests above, run from build/ via ctest.
- Loopback test (scripts/loopback_test.sh): manager configs/itch-null.cfg (ITCH feed, strategy
  dummy, null mode) for 20M events, then SIGTERM. Assert from strategy_stats.log and the stats
  blocks: orders > 0, acks == orders minus risk rejects, fills == 0, open_orders == 0 after
  shutdown, risk_rejects present but ring_full_rejects == 0, frame_gaps == 0, tick_to_order p99 < 2
  us. Second run with strategy_dummy.so must match the baked binary's order count. Compare
  core_main's e2e and process histograms against benchmarks/benchmarks.md (153fec8) with the logging
  strategy to prove no regression.
- Swap test: kill -USR1 the manager while the pipeline runs; exec restarts with the strategy named
  in Control.swap_path, core_main's event_seq continues uninterrupted, exec_generation increments.
- FIX unit and replay: byte-exact logon signature against a known vector; W and X parsing from
  recorded sandbox captures; session gap to ResendRequest to GapFill over a socketpair peer.
- Coinbase sandbox checklist: logon both sessions, L2 subscribe BTC-USD, the dashboard book matches
  the REST /products/BTC-USD/book?level=2 snapshot, heartbeats steady for 10 minutes with seq_gaps
  == 0; null mode on sandbox data shows strategy orders acked locally; then live: place and cancel a
  0.0001 BTC post-only order far from touch and see Ack and Canceled exec reports with a sane
  ack_rtt; dashboard KILL rejects new orders and cancels open ones. Failure drills: kill -9
  exec_main and the gateway cancels all within 2 s; kill -9 gateway_main and the venue cancels via
  8013=Y (verify in the sandbox UI); pull the network and see reconnect, backoff and a FEED_RESET;
  SIGUSR1 exec restart replays open orders as Restated and adopts them. Record both sessions with
  RecordingTransport and promote scrubbed captures to tests/fixtures/.
- Kraken checklist (key with query and trade permissions only): token fetched and refreshed;
  instrument channel populates the table with correct tick and lot; BTC/USD book depth 10 runs
  for 30 minutes with zero checksum failures and the dashboard book matches the Kraken Pro UI.
  validate=true: limit, post-only, IOC, amend,
  cancel all return success and an oversized order returns insufficient funds as a mapped
  Reject. Live: one minimum-size post-only order far from touch, Ack then Canceled with sane
  ack_rtt; amend keeps the order_id; executions map back to cl_ord_id; GatewayStats rate budget
  agrees with ratecounter. Drills: KILL cancels via cancel_all; kill -9 gateway_main and the dead
  man's switch cancels within 60 s (verify in the UI); a sell larger than base holdings and an
  order on a non-allowlisted asset are rejected by exec, never sent.
- Research: convert_kraken.py replays a recorded day with zero checksum failures; rate_model.py
  and test_rate_model agree on the shared vectors; the calibration report exists for every
  live strategy before its size is raised.
- Dashboard: every new panel is non-empty in null mode (fill and PnL panels once orders fill in
  the sandbox); the kill button flips KILL and the gateway logs cancel-all.

## Risks and notes

- Live-money safety: Phases 2 to 4 never touch real money (ITCH replay, then Coinbase sandbox).
  Kraken requires explicit config (live=1), defaults to PAUSE until the dashboard releases it,
  and starts with minimum sizes and small balances on the account.
- Jurisdiction: venues are chosen for an Ontario retail account. Coinbase Exchange FIX is
  institutional-only, so it stays a sandbox; Hyperliquid restricts Ontario. Never route around a
  venue's geographic restrictions with a VPN or proxy. Kraken FIX (institutional, by request to
  an account manager) is a possible later upgrade that would reuse the FIX codec and session.
- EXEC_CORE=3 is the HT sibling of EXCHANGE_CORE=2 in ITCH mode, as config.hpp documents; in
  live mode exec takes core 4. Cores are config-driven so the trade-off is per machine.
- The wider frame (576 B at depth 10) costs the ITCH path an extra copy per event; Phase 1 keeps
  the benchmark comparison and Phase 7 can instantiate depth 5 for ITCH if it shows.
- Silent frame drops on the feed ring exist today; after Phase 1 they are counted and signalled
  so strategies know their view of the book was interrupted.
- This NFS box lacks OpenSSL headers, glfw3, hugepages (HugePages_Total 0) and the ITCH feed.
  With the hugepage fallback and LL_BUILD_LIVE/LL_BUILD_DASHBOARD off, Phases 1 and 2 build and
  their tests run here; live phases need the 6C/12T machine (sudo apt install libssl-dev).
- Existing quirks to fix while touching the code: Event is 128 B not 64 B (the _pad[2] is
  misleading); the ITCH parser back-pressures on EXCHANGE_RING_CAPACITY (4096) while writing an
  8192 ring, so queue depth never exceeds 50%; the parser widens tail to uint64_t (wraps after
  2^32 messages); Seqlock::store fence ordering is x86-only; Histogram percentiles are
  power-of-two upper bounds (add log-linear sub-buckets if sub-100 ns resolution matters); the
  manager's exec_path buffer is 64 chars.
- Kraken has no spot sandbox. validate=true and the Coinbase sandbox are the only free tests,
  so the Kraken message mapping must be covered by recorded-capture tests before step (c).
- Kraken's rate counter penalizes cancelling young orders, which is exactly what a naive
  quoter does. A strategy tuned on ITCH without the rate model will be throttled live; running
  the rate model in every mode exists to catch this before real orders.
- Backtests are estimates even with L3 data (hidden and iceberg orders, our own impact). The
  Coinbase sandbox validates mechanics, not profitability (its book is a synthetic mirror of
  production). Profitability is only measured by minimum-size live trading on Kraken, and the
  calibration step turns that into an error bar on hftbacktest results.
- Fees dominate: at retail Kraken tiers a maker round trip costs on the order of 18 bp against a
  BTC/USD spread far below 1 bp, so pure spread capture cannot pay. Every backtest runs with the
  account's real fee tier.
- Kraken WS tokens expire 15 minutes after creation; confirm whether an established private
  connection survives expiry, and treat token refresh failure as a session fault (PAUSE).
- cl_ord_id embeds an exec epoch so a restarted exec never reuses ids the venue still knows; the
  gateway's OrderStateTable persists across exec restarts.
- -march=native makes strategy modules valid only on the machine that built them; the loader's
  CPU-feature check and build_id turn a SIGILL into a clear error.
- New code parses numbers with strtol and strtod, not std::stoi, which std::terminates under
  -fno-exceptions as the existing argv[1] parsing already would.
- Coinbase sessions are not resumable across the weekly reset or a disconnect. Recovery is always
  OrderStateTable plus 8013=Y plus cancel-all on doubt, never FIX resend of application messages.


## Implementation breakdown

The Phasing section above sets scope; this section orders the work. Each numbered step is one
reviewable change (roughly one commit) that builds, passes every existing test, and leaves the
ITCH pipeline runnable. "Done when" is the exit check for the step or phase. Phase 3 does not
depend on Phase 2 and can be worked in parallel with it; step 5.2 (Kraken market data) needs only
Phase 3 plus steps 4.1 and 4.2, so it can be pulled forward to get live data early.

### Phase 0: prerequisites (no pipeline code)

0.1 Commit plan.md and CLAUDE.md.
0.2 Benchmark HEAD (2cd81d8) with the logging strategy, 20M events, and add the benchmarks.md
    entry and plot. Every later hot-path change is compared against this, not 153fec8.
0.3 Fix the plan's errata: the Risks note describing an NFS box (this machine is the Fedora
    6C/12T target; it needs `dnf install openssl-devel`, not apt), and the live-mode core map
    that puts the gateway OE thread on non-isolated CPU 8.
0.4 Accounts, in the background since they have lead time: Coinbase Exchange sandbox login and
    an API key with trade permission (confirm FIX logon is allowed); Kraken account verified to
    at least Intermediate, an API key with query and trade permissions only, the tier's rate
    counter max and decay noted for config, a small fiat balance.
Done when: baseline entry exists for 2cd81d8 and both venue keys are in hand.

### Phase 1: contracts and shm v3

1.1 Test plumbing: extract tests/check.hpp from test_orderbook.cpp, enable_testing(), register
    the existing tests with ctest (label test_itch_parser slow since it replays the whole feed).
1.2 SpscRing try_claim()/publish(); move ItchParser::next() onto it, fix its back-pressure bound
    to the ring it actually writes, and stop widening tail to uint64_t. Add a small ring test.
1.3 ShmSegment::create() falls back from MFD_HUGETLB to normal pages with a warning.
1.4 Mechanical rename: exec::ExecRing to FeedRing, core_to_exec to feed_to_exec.
1.5 src/common/ipc/instrument.hpp (64 B Instrument, InstrumentTable); core_main registers the
    ITCH symbol (tick 0.01, lot 1) at startup.
1.6 Delete the shadow FillModel: fill_model.hpp, its use in runner.hpp, strategy.hpp and
    exec_main.cpp, and EXEC_LATENCY_NS. Until NullRouter lands in 2.5, StrategyContext::submit
    hands out ids and tracks live orders locally, and nothing fills. Done when exec_main_dummy
    runs and logs its order count for a fixed event count (the reference for 1.7).
1.7 MarketUpdate v2: int64 ticks and lots, instrument, flags, EXEC_DEPTH 10. Carry the
    mechanical int64 change through MarketView, StrategyContext, both strategies and
    make_market_update (4 dp to ticks). Done when DummyQuoter's order count and quote prices on
    the same event count match 1.6.
1.8 Wire types with no producers yet: order_msgs.hpp, control.hpp, stats.hpp. PipelineShm v3
    with ipc::VERSION 3 and a constexpr layout_hash; every worker checks both and aborts with a
    message. test_wire_layout pins sizeof/offsetof goldens.
1.9 Feed integrity: core_main counts ring-full drops in FeedStats and sets FEED_RESET on the next
    frame; exec counts event_seq gaps into ExecStats.frame_gaps. Push the exec frame before the
    dashboard seqlock store. Treat ITCH 'Q' cross trades like 'P'.
1.10 SnapshotV2 (int64 levels, instrument, price_exp); dashboard renders from it.
Done when: all tests pass, the manager runs end to end with exec_main_dummy as before, and a
benchmark entry against the Phase 0 baseline shows the cost of the 576 B frame.

### Phase 2: exec as a trading engine on ITCH

Engine core:
2.1 src/common/config_file.hpp key=value parser plus test_kv_config.
2.2 account.hpp (positions, balances available and reserved, PnL, fees, drawdown) plus
    test_account.
2.3 order_table.hpp (4096 slots open-addressed by cl_ord_id) plus test_order_table.
2.4 risk.hpp: size, notional, position and open-order limits, price band, KILL and PAUSE,
    spot balance checks, asset allowlist, and the venue rate model (Kraken counter with
    age-dependent cancel and amend penalties). test_risk_gate and test_rate_model.
2.5 Router concept and NullRouter; ExecContext; StrategyContext<Router> with the extended API
    (position, open_orders, replace, cancel_all, rate_budget) and requires-detected optional
    hooks; run_strategy<S, Router> returning RunExit, with feed_transit, tick and tick_to_order
    histograms and periodic ExecStats, PositionStats and OpenOrders publishing. Both existing
    strategies compile unchanged.
2.6 exec_main --mode/--strategy/--config, the shutdown sequence and strategy_stats.log;
    configs/itch-null.cfg and scripts/loopback_test.sh asserting the Verification criteria.
2.7 imbalance_taker strategy.
Done when: loopback_test.sh passes with dummy and imbalance_taker.

Strategy modules:
2.8 module_abi.hpp, LL_STRATEGY_MODULE, the builtin registry, so exec_main_<name> goes through
    the same table.
2.9 loader (memfd copy, dlopen, abi_version, layout_hash, build_id and CPU feature checks,
    pre-fault), strategy_<name>.so targets with export.map, check_strategy_so.sh post-build, and
    the generic exec_main. test_module_abi.
Done when: the .so and baked runs of dummy produce identical order counts.

Manager and dashboard:
2.10 manager reads pipeline.cfg (workers, cores, strategy, feed path) instead of the inline
    array and EXEC_STRATEGY; SIGUSR1 restarts only exec; exec death sets KILL and optionally
    respawns; core death sets KILL and PAUSE.
2.11 Dashboard: status strip with kill and pause buttons, PnL and position table with
    balances, open orders, execution quality (fill-based panels such as markouts stay empty
    until the sandbox produces fills in Phase 4), tick-to-order and decomposition latency
    panels, ring and gap health.
Done when: the Phase 2 deliverable holds and the swap test passes.

### Phase 3: transport (parallel with Phase 2; needs openssl-devel)

3.1 CMake: LL_BUILD_LIVE (OpenSSL, simdjson via FetchContent) and LL_BUILD_DASHBOARD options;
    ll_common, ll_exec and ll_gateway_net targets. The ITCH-only build stays OpenSSL-free.
3.2 decimal.hpp plus test_decimal (has no OpenSSL dependency; can land any time).
3.3 tcp.hpp and TlsSocket. Smoke test: TLS handshake with verification to a public host.
3.4 HttpsClient. Smoke test: Coinbase sandbox GET /products and Kraken GET /0/public/Time.
3.5 WsClient plus test_ws_frame. Smoke test: subscribe to Kraken's public ticker and print.
3.6 crc32.hpp and the simdjson wrappers with raw-number-token decimal parsing.
3.7 fix::Encoder and fix::Decoder plus test_fix_codec.
3.8 FixSession over MemoryTransport plus test_fix_session; RecordingTransport and the replay
    harness.
Done when: all transport tests pass and the three smoke tests run against real endpoints.

### Phase 4: Coinbase sandbox (test venue)

4.1 TopNBook plus test_topn_book against a std::map oracle.
4.2 gateway_main skeleton: args, config, shm attach and checks, pinned MD and OE threads, the
    housekeeping thread, run_gateway<Venue> dispatch, FeedStats and GatewayStats publishing;
    manager live mode (no exchange_main or core_main; exec on its live core).
4.3 coinbase_fix MD: logon, L2 subscribe, W and X into TopNBook and frames, instrument table
    from REST /products, RptSeq-gap resync with FEED_RESET; the gateway-side dashboard
    snapshotter. Checkpoint: exec in null mode on sandbox data, dashboard book matches REST.
4.4 Gateway OrderStateTable; GatewayRouter and live mode in exec; Account seeded from REST
    /accounts.
4.5 coinbase_fix OE: logon with 8013=Y, D, F and G encoding, 8 and 9 decoding into ExecReport,
    per-order cancel_all, KILL and stale-exec-heartbeat handling.
4.6 Exec restart handoff: exec_generation, Restated replay, adopt or cancel by strategy_tag.
4.7 Dashboard venue RTT and session-health strips.
4.8 Run the Coinbase sandbox checklist and failure drills; record both sessions and promote
    scrubbed captures to tests/fixtures/.
Done when: the full sandbox checklist passes.

### Phase 5: Kraken (live venue)

5.1 Kraken REST signing, GetWebSocketsToken and Balance on the housekeeping thread, token
    handoff to the OE thread via seqlock, refresh before expiry. test_kraken_sign with the
    documented known-answer vector. Resolve the open item on token expiry for open connections.
5.2 kraken_ws MD: instrument channel into the InstrumentTable, book depth 10 with CRC32
    verification and resubscribe on mismatch, trade frames. test_kraken_checksum. Stage (a):
    exec in null mode on live Kraken data for 30 minutes with zero checksum failures.
5.3 kraken_ws OE: executions (snap_orders reconciliation) and balances, add_order, amend_order,
    cancel_order and cancel_all, error-to-reject_reason mapping, ratecounter into GatewayStats,
    cancel_all_orders_after armed and refreshed. test_kraken_messages from recorded captures.
5.4 Live gating: live=1 required, PAUSE at start, allowlist and rate values from config.
5.5 Stage (b): every order type and error path with validate=true.
5.6 Stage (c): one minimum-size post-only order far from touch, then the Kraken drills (KILL,
    kill -9 gateway with the dead man's switch, rejected oversell and non-allowlisted asset).
5.7 Stage (d): a quoting strategy at minimum size with tight limits, chosen from hftbacktest
    research (R.3); watch PnL, markouts and rate budget on the dashboard. Run calibration (R.5)
    on these sessions.
Done when: the Kraken checklist passes and stage (d) runs a full session without rate-limit
rejects or manual intervention.

### Research track: hftbacktest (parallel from Phase 3)

Lives in research/ (Python) plus one C++ tool. It never blocks the trading phases, but R.3 should
have produced a candidate strategy before Kraken stage (d).

R.1 kraken_recorder (needs Phase 3 transport): public book and trade subscriptions, raw
    messages with local receive timestamps, rotating compressed files, unpinned. Start it
    running continuously as soon as it works; history accumulates from that day.
R.2 research/ setup: requirements.txt pinning hftbacktest, convert_kraken.py (raw captures to
    hftbacktest events, replaying the book checksum as validation), and rate_model.py with the
    shared test vectors that test_rate_model also checks. Optionally buy a Tardis sample of
    Kraken BTC/USD to compare against our own recordings for the same period.
R.3 First backtests: BTC/USD with trading_value_fee_model at the account's tier (and the next
    tier), a probabilistic queue model on L2 data, constant latency from measured public RTT.
    Port the dummy quoter first as a sanity check, then prototype real candidates. Expect the
    fee wall to rule out pure spread capture; that result is useful on its own.
R.4 Level 3 and measured latency (needs 5.1 for the token, and 4.5 or 5.3 for order timings):
    kraken_recorder adds the authenticated level3 channel; research switches to
    l3_fifo_queue_model. The gateway logs per-order request, venue and receive timestamps;
    convert_latency.py feeds intp_order_latency.
R.5 Calibration (needs 5.6 or 5.7 sessions): replay recorded sessions in hftbacktest with the
    logged order actions and compare predicted against actual fills per order; report the
    error in fill probability, time to fill and PnL. Repeat as live sessions accumulate.
Done when: a strategy's live minimum-size PnL falls within the calibrated error bar of its
backtest, or the calibration shows which model assumption to fix.

### Phase 6: hot-swap and hardening

6.1 In-process hot-swap on SIGHUP or Control.SWAP with state export and import.
6.2 Chaos tests on the Coinbase sandbox (kill exec mid-session, drop sockets, resync), then the
    kill drills once more on Kraken at minimum size.
6.3 Socket and host tuning: SO_BUSY_POLL, TCP_QUICKACK, interrupt coalescing, NIC IRQ steering;
    measure each.
6.4 README (architecture diagram, live mode) and benchmarks.md with a tick-to-order section,
    keeping the ITCH numbers comparable.

### Phase 7: benchmark-gated optimizations

One step per item, each with a before and after benchmarks.md entry, kept only if it wins:
7.1 Repack common::Event to 64 B.
7.2 Producer-side cached head in SpscRing.
7.3 EXEC_DEPTH 5 instantiation for the ITCH path.
7.4 Skip frames when neither the top EXEC_DEPTH levels nor the trade changed.
7.5 Capture the dashboard frame on demand instead of per event.
7.6 Log-linear histogram sub-buckets.

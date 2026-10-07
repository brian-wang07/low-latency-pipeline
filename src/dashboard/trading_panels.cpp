#include "dashboard/trading_panels.hpp"

#include "common/histogram.hpp"
#include "common/ipc/stats.hpp"
#include "common/platform/tsc.hpp"
#include "exec/risk.hpp"
#include "imgui.h"
#include "implot.h"
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace dashboard {

namespace {

constexpr int N = 240;              // samples kept per series
constexpr double SAMPLE_S = 0.25;   // sampling period for rates and percentiles
constexpr float WINDOW_S = 60.f;    // plotted history

const ImVec4 RED(0.90f, 0.25f, 0.25f, 1.f);
const ImVec4 GREEN(0.30f, 0.80f, 0.40f, 1.f);
const ImVec4 AMBER(0.95f, 0.70f, 0.20f, 1.f);
const ImVec4 GREY(0.60f, 0.60f, 0.60f, 1.f);

double money(int64_t v) { return double(v) / 1e8; }

// Percentile (ns) of the samples recorded between two cumulative histograms.
double delta_pct(const common::Histogram &now, const common::Histogram &prev,
                 double p, double tsc_per_ns) {
  common::Histogram d{};
  for (int b = 0; b < 64; ++b)
    d.buckets[b] = now.buckets[b] - prev.buckets[b];
  d.count = now.count - prev.count;
  return d.count ? double(d.percentile_cycles(p)) / tsc_per_ns : NAN;
}

} // namespace

struct TradingPanels::Series {
  float t[N]{};
  float v[4][N]{};
  int head = 0, size = 0;
  void push(float tt, float a, float b = NAN, float c = NAN, float d = NAN) {
    t[head] = tt;
    v[0][head] = a;
    v[1][head] = b;
    v[2][head] = c;
    v[3][head] = d;
    head = (head + 1) % N;
    if (size < N)
      ++size;
  }
  int offset() const { return size < N ? 0 : head; }
};

struct TradingPanels::State {
  ipc::PipelineShm *p;
  double tsc_per_ns = 1.0;
  stats::ExecStats exec{}, exec_prev{};
  stats::FeedStats feed{}, feed_prev{};
  stats::PositionStats pos{};
  stats::BalanceStats bal{};
  stats::OpenOrders oo{};
  bool have_exec = false, have_feed = false;
  double last_sample = 0;
  double frames_per_s = 0, iters_per_s = 0, events_per_s = 0;
  Series t2o, transit, tick, rings, equity;
  char swap_buf[sizeof(ipc::Control::swap_path)] = {};
};

TradingPanels::TradingPanels(ipc::PipelineShm *p) noexcept : s_(new State{}) {
  s_->p = p;
  calibrate_tsc(s_->tsc_per_ns);
}

void TradingPanels::update(double t_now) noexcept {
  State &s = *s_;
  stats::ExecStats e;
  if (s.p->exec_stats.try_load(e)) {
    s.exec = e;
    s.have_exec = e.exec_generation != 0;
  }
  stats::FeedStats f;
  if (s.p->feed_stats.try_load(f)) {
    s.feed = f;
    s.have_feed = f.mode != stats::FEED_NONE;
  }
  stats::PositionStats ps;
  if (s.p->positions.try_load(ps))
    s.pos = ps;
  stats::BalanceStats bs;
  if (s.p->balances.try_load(bs))
    s.bal = bs;
  stats::OpenOrders oo;
  if (s.p->open_orders.try_load(oo))
    s.oo = oo;

  const double dt = t_now - s.last_sample;
  if (dt < SAMPLE_S)
    return;
  const float t = float(t_now);
  // A new exec generation restarts its counters: don't diff across it.
  if (s.exec.exec_generation != s.exec_prev.exec_generation)
    s.exec_prev = s.exec;
  s.frames_per_s = double(s.exec.frames - s.exec_prev.frames) / dt;
  s.iters_per_s = double(s.exec.iterations - s.exec_prev.iterations) / dt;
  s.events_per_s = double(s.feed.events_in - s.feed_prev.events_in) / dt;
  const double k = s.tsc_per_ns;
  s.t2o.push(t, float(delta_pct(s.exec.tick_to_order, s.exec_prev.tick_to_order, 0.5, k)),
             float(delta_pct(s.exec.tick_to_order, s.exec_prev.tick_to_order, 0.99, k)),
             float(delta_pct(s.exec.tick_to_order, s.exec_prev.tick_to_order, 0.999, k)));
  s.transit.push(t, float(delta_pct(s.exec.feed_transit, s.exec_prev.feed_transit, 0.5, k)),
                 float(delta_pct(s.exec.feed_transit, s.exec_prev.feed_transit, 0.99, k)));
  s.tick.push(t, float(delta_pct(s.exec.tick, s.exec_prev.tick, 0.5, k)),
              float(delta_pct(s.exec.tick, s.exec_prev.tick, 0.99, k)));
  auto occ = [](uint32_t size, uint32_t cap) { return 100.f * float(size) / float(cap); };
  s.rings.push(t, occ(s.p->exchange_to_core.size(), core::CORE_RING_CAPACITY),
               occ(s.p->feed_to_exec.size(), exec::FEED_RING_CAPACITY),
               occ(s.p->exec_to_gateway.size(), oe::ORDER_RING_CAPACITY),
               occ(s.p->gateway_to_exec.size(), oe::EXEC_RING_CAPACITY));
  int64_t realized = 0, unrealized = 0, fees = 0;
  for (uint32_t i = 0; i < s.pos.count; ++i) {
    realized += s.pos.rows[i].realized_pnl;
    unrealized += s.pos.rows[i].unrealized_pnl;
    fees += s.pos.rows[i].fees;
  }
  s.equity.push(t, float(money(realized + unrealized - fees)), float(money(realized)));
  s.exec_prev = s.exec;
  s.feed_prev = s.feed;
  s.last_sample = t_now;
}

void TradingPanels::render_status_strip() noexcept {
  State &s = *s_;
  ipc::Control &ctl = s.p->control;
  const uint32_t flags = ctl.flags.load(std::memory_order_acquire);
  const uint64_t now = read_tsc();
  auto age_ms = [&](uint64_t tsc) {
    return tsc ? double(now - tsc) / s.tsc_per_ns / 1e6 : NAN;
  };

  // Every segment starts at a fixed column and numbers have fixed widths, so
  // values gaining or losing digits never shift the rest of the strip.
  const float cw = ImGui::CalcTextSize("0").x; // monospace cell
  const float x0 = ImGui::GetCursorPosX();
  auto at = [&](int col) { ImGui::SameLine(x0 + float(col) * cw); };
  auto clamp = [](double v, double hi) { return std::isnan(v) ? v : v > hi ? hi : v; };

  if (s.have_exec)
    ImGui::Text("Exec %s  %-16.16s gen %-5" PRIu64,
                s.exec.mode == stats::EXEC_LIVE ? "LIVE" : "NULL", s.exec.strategy_name,
                s.exec.exec_generation);
  else
    ImGui::TextColored(GREY, "Exec: no stats yet");
  const double exec_age = age_ms(s.exec.heartbeat_tsc);
  at(40);
  ImGui::TextColored(!s.have_exec || exec_age > 500 ? RED : GREEN, "exec hb %6.0f ms",
                     clamp(exec_age, 999999));
  const double feed_age = age_ms(s.feed.heartbeat_tsc);
  at(59);
  ImGui::TextColored(!s.have_feed || feed_age > 2000 ? AMBER : GREEN, "feed hb %6.0f ms",
                     clamp(feed_age, 999999));
  at(78);
  ImGui::Text("feed %9.0f ev/s", clamp(s.events_per_s, 999999999));
  at(99);
  ImGui::Text("exec %7.0f frames/s", clamp(s.frames_per_s, 9999999));
  at(122);
  ImGui::Text("%8.2fM iter/s", clamp(s.iters_per_s / 1e6, 99999.99));

  // Toggle buttons sized to their longer label so toggling doesn't move anything.
  const ImVec2 btn(ImGui::CalcTextSize("PAUSED (resume)").x +
                       2 * ImGui::GetStyle().FramePadding.x,
                   0);
  at(139);
  const bool kill = flags & ipc::KILL, pause = flags & ipc::PAUSE;
  ImGui::PushStyleColor(ImGuiCol_Button, kill ? RED : ImVec4(0.25f, 0.25f, 0.25f, 1.f));
  if (ImGui::Button(kill ? "KILL ON (clear)" : "KILL", btn)) {
    if (kill)
      ctl.flags.fetch_and(~uint32_t(ipc::KILL), std::memory_order_acq_rel);
    else
      ctl.flags.fetch_or(ipc::KILL, std::memory_order_acq_rel);
  }
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, pause ? AMBER : ImVec4(0.25f, 0.25f, 0.25f, 1.f));
  if (ImGui::Button(pause ? "PAUSED (resume)" : "PAUSE", btn)) {
    if (pause)
      ctl.flags.fetch_and(~uint32_t(ipc::PAUSE), std::memory_order_acq_rel);
    else
      ctl.flags.fetch_or(ipc::PAUSE, std::memory_order_acq_rel);
  }
  ImGui::PopStyleColor();

  ImGui::SameLine(0, 32);
  ImGui::SetNextItemWidth(260);
  ImGui::InputTextWithHint("##swap", "builtin:<name> or ./strategy_<name>.so", s.swap_buf,
                           sizeof(s.swap_buf));
  ImGui::SameLine();
  if (ImGui::Button("Swap strategy") && s.swap_buf[0]) {
    // exec exits with the swap status; the manager respawns it with swap_path.
    std::memcpy(ctl.swap_path, s.swap_buf, sizeof(ctl.swap_path));
    ctl.flags.fetch_or(ipc::SWAP, std::memory_order_acq_rel);
  }
}

void TradingPanels::render_trading_tab() noexcept {
  State &s = *s_;
  const stats::ExecStats &e = s.exec;
  const ImGuiTableFlags tf = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                             ImGuiTableFlags_SizingStretchProp;

  // Positions with utilization of the position limit.
  ImGui::SeparatorText("Positions");
  if (ImGui::BeginTable("pos", 9, tf)) {
    for (const char *h : {"Instrument", "Qty", "Avg px", "Realized", "Unrealized",
                          "Fees", "Bought", "Sold", "Limit use"})
      ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (uint32_t i = 0; i < s.pos.count; ++i) {
      const stats::Position &r = s.pos.rows[i];
      const ref::Instrument &inst = s.p->instruments.rows[r.instrument];
      const double px_scale = std::pow(10.0, inst.tick_exp) * double(inst.tick_mant);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%s/%s", inst.symbol, inst.venue);
      ImGui::TableNextColumn();
      ImGui::Text("%" PRId64, r.qty_lots);
      ImGui::TableNextColumn();
      ImGui::Text("%.4f", double(r.avg_px_ticks) * px_scale);
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", money(r.realized_pnl));
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", money(r.unrealized_pnl));
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", money(r.fees));
      ImGui::TableNextColumn();
      ImGui::Text("%" PRId64, r.bought);
      ImGui::TableNextColumn();
      ImGui::Text("%" PRId64, r.sold);
      ImGui::TableNextColumn();
      if (e.limit_max_position > 0) {
        const float use = float(std::fabs(double(r.qty_lots)) / double(e.limit_max_position));
        char lbl[32];
        std::snprintf(lbl, sizeof(lbl), "%.0f%%", use * 100.f);
        ImGui::ProgressBar(use > 1.f ? 1.f : use, ImVec2(-1, 0), lbl);
      } else {
        ImGui::TextColored(GREY, "no limit");
      }
    }
    ImGui::EndTable();
  }
  if (s.pos.count) {
    const stats::Position &r0 = s.pos.rows[0];
    ImGui::Text("Equity %.2f   peak %.2f   max drawdown %.2f",
                s.equity.size ? s.equity.v[0][(s.equity.head + N - 1) % N] : 0.f,
                money(r0.peak_equity), money(r0.max_drawdown));
  }

  // Balances.
  ImGui::SeparatorText("Balances");
  if (ImGui::BeginTable("bal", 4, tf)) {
    for (const char *h : {"Asset", "Total", "Reserved", "Available"})
      ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for (uint32_t i = 0; i < s.bal.count; ++i) {
      const stats::AssetBalance &b = s.bal.assets[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%.8s", b.name);
      ImGui::TableNextColumn();
      ImGui::Text("%.4f", money(b.total));
      ImGui::TableNextColumn();
      ImGui::Text("%.4f", money(b.reserved));
      ImGui::TableNextColumn();
      ImGui::Text("%.4f", money(b.total - b.reserved));
    }
    ImGui::EndTable();
  }

  // Execution quality.
  ImGui::SeparatorText("Execution");
  const double sent = double(e.orders_new);
  ImGui::Text("orders %" PRIu64 "  amends %" PRIu64 "  cancels %" PRIu64
              "   acks %" PRIu64 "  replaced %" PRIu64 "  canceled %" PRIu64
              "  expired %" PRIu64 "  venue rejects %" PRIu64,
              e.orders_new, e.orders_replace, e.orders_cancel, e.acks, e.replaced,
              e.canceled, e.expired, e.venue_rejects);
  ImGui::Text("amend/order %.2f   cancel/order %.2f   risk rejects %" PRIu64
              "   ring-full rejects %" PRIu64,
              sent ? double(e.orders_replace) / sent : 0.0,
              sent ? double(e.orders_cancel) / sent : 0.0, e.risk_rejects,
              e.ring_full_rejects);
  if (e.fills == 0)
    ImGui::TextColored(GREY, "fills, fill ratio, maker/taker, markouts: no fills (null mode)");
  else
    ImGui::Text("fills %" PRIu64 "  fill ratio %.3f", e.fills, sent ? double(e.fills) / sent : 0.0);
  ImGui::Text("rate budget:");
  for (int k = 0; k < 4 && k < int(s.pos.count); ++k) {
    ImGui::SameLine();
    ImGui::TextColored(e.rate_budget[k] < 5 ? RED : GREEN, " %s %.1f",
                       s.p->instruments.rows[s.pos.rows[k].instrument].symbol,
                       e.rate_budget[k]);
  }

  const float half = ImGui::GetContentRegionAvail().x * 0.5f - 4.f;
  ImGui::BeginChild("##rejects", ImVec2(half, 0), true);
  ImGui::TextUnformatted("Rejects by reason");
  if (ImGui::BeginTable("rej", 2, tf)) {
    for (int r = 1; r < int(exec::RejectReason::Count); ++r) {
      if (!e.rejects[r])
        continue;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(exec::reject_name(exec::RejectReason(r)));
      ImGui::TableNextColumn();
      ImGui::Text("%" PRIu64, e.rejects[r]);
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
  ImGui::SameLine();

  // Open orders, oldest first.
  ImGui::BeginChild("##orders", ImVec2(0, 0), true);
  ImGui::Text("Open orders (%u shown, %u live)", s.oo.count, e.open_orders);
  if (ImGui::BeginTable("oo", 6, tf | ImGuiTableFlags_ScrollY)) {
    for (const char *h : {"Id", "Side", "Px", "Qty", "Leaves", "Age ms"})
      ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    const uint64_t now = read_tsc();
    for (uint32_t i = 0; i < s.oo.count; ++i) {
      const stats::OpenOrder &o = s.oo.rows[i];
      const ref::Instrument &inst = s.p->instruments.rows[o.instrument];
      const double px_scale = std::pow(10.0, inst.tick_exp) * double(inst.tick_mant);
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%" PRIx64, uint64_t(o.cl_ord_id & 0xffffffffffull));
      ImGui::TableNextColumn();
      ImGui::TextColored(o.side == common::Side::Buy ? GREEN : RED, "%s",
                         o.side == common::Side::Buy ? "BUY" : "SELL");
      ImGui::TableNextColumn();
      ImGui::Text("%.4f", double(o.px) * px_scale);
      ImGui::TableNextColumn();
      ImGui::Text("%" PRId64, o.qty);
      ImGui::TableNextColumn();
      ImGui::Text("%" PRId64, o.leaves);
      ImGui::TableNextColumn();
      ImGui::Text("%.0f", double(now - o.tsc_sent) / s.tsc_per_ns / 1e6);
    }
    ImGui::EndTable();
  }
  ImGui::EndChild();
}

void TradingPanels::render_health_tab() noexcept {
  State &s = *s_;
  const float w = ImGui::GetContentRegionAvail().x * 0.5f - 4.f;
  const float h = ImGui::GetContentRegionAvail().y * 0.5f - 8.f;
  const float t_now = s.t2o.size ? s.t2o.t[(s.t2o.head + N - 1) % N] : 0.f;

  auto plot = [&](const char *title, const Series &sr, const char *const *labels, int n,
                  const char *y_label, bool log_y) {
    if (!ImPlot::BeginPlot(title, ImVec2(w, h), ImPlotFlags_NoMenus))
      return;
    ImPlot::SetupAxes("s", y_label, 0, ImPlotAxisFlags_AutoFit);
    if (log_y)
      ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);
    ImPlot::SetupAxisLimits(ImAxis_X1, t_now - WINDOW_S, t_now, ImGuiCond_Always);
    for (int k = 0; k < n; ++k)
      ImPlot::PlotLine(labels[k], sr.t, sr.v[k], sr.size, 0, sr.offset());
    ImPlot::EndPlot();
  };

  static const char *const PCT[] = {"p50", "p99", "p99.9"};
  plot("tick-to-order (ns, per 250 ms)", s.t2o, PCT, 3, "ns", true);
  ImGui::SameLine();
  static const char *const DECOMP[] = {"feed transit p50", "feed transit p99"};
  plot("feed transit: parser -> exec pop (ns)", s.transit, DECOMP, 2, "ns", true);

  static const char *const TICK[] = {"on_tick p50", "on_tick p99"};
  plot("on_tick duration (ns)", s.tick, TICK, 2, "ns", true);
  ImGui::SameLine();
  ImGui::BeginGroup();
  static const char *const RINGS[] = {"exchange->core", "feed->exec", "order->gateway",
                                      "reports->exec"};
  if (ImPlot::BeginPlot("ring occupancy (%)", ImVec2(w, h * 0.7f), ImPlotFlags_NoMenus)) {
    ImPlot::SetupAxes("s", "%", 0, 0);
    ImPlot::SetupAxisLimits(ImAxis_Y1, 0, 100, ImGuiCond_Always);
    ImPlot::SetupAxisLimits(ImAxis_X1, t_now - WINDOW_S, t_now, ImGuiCond_Always);
    for (int k = 0; k < 4; ++k)
      ImPlot::PlotLine(RINGS[k], s.rings.t, s.rings.v[k], s.rings.size, 0, s.rings.offset());
    ImPlot::EndPlot();
  }
  const bool gaps_ok = s.exec.frame_gaps == 0 && s.feed.frames_dropped == 0;
  ImGui::TextColored(gaps_ok ? GREEN : RED,
                     "feed: %" PRIu64 " frames pushed, %" PRIu64 " dropped   exec: %" PRIu64
                     " frames, %" PRIu64 " gap frames",
                     s.feed.frames_pushed, s.feed.frames_dropped, s.exec.frames,
                     s.exec.frame_gaps);
  ImGui::EndGroup();
}

} // namespace dashboard

#pragma once

#include "common/ipc/order_msgs.hpp"
#include "common/ipc/shm.hpp"
#include "common/platform/tsc.hpp"
#include <concepts>
#include <cstdint>

namespace exec {

struct ReportSink {
  void operator()(const oe::ExecReport &) const noexcept {}
};

// Where exec's order requests go and where execution reports come back from.
// submit() returns false when the request can't be queued (ring full). poll()
// hands every pending report to the callback; reports for a request are never
// delivered from inside submit(), so strategies are not re-entered.
template <class R>
concept Router = requires(R r, const oe::OrderRequest &req,
                          const MarketUpdate<EXEC_DEPTH> &u, ReportSink sink) {
  { r.submit(req) } noexcept -> std::same_as<bool>;
  { r.on_frame(u) } noexcept;
  { r.poll(sink) } noexcept;
};

// ITCH mode: no venue. Every request is accepted and answered on the next poll;
// nothing ever fills. New -> Ack (IOC/FOK then Expired), Cancel -> Canceled,
// Replace -> Replaced.
class NullRouter {
public:
  bool submit(const oe::OrderRequest &req) noexcept {
    const int needed =
        (req.type == oe::ReqType::New &&
         (req.tif == oe::Tif::IOC || req.tif == oe::Tif::FOK))
            ? 2
            : 1;
    if (QUEUE - (tail_ - head_) < uint32_t(needed))
      return false;
    oe::ExecReport r{};
    r.cl_ord_id = req.cl_ord_id;
    r.orig_cl_ord_id = req.orig_cl_ord_id;
    r.strategy_tag = req.strategy_tag;
    r.tsc_sent = req.tsc_decision;
    r.instrument = req.instrument;
    switch (req.type) {
    case oe::ReqType::New:
      r.exec_type = oe::ExecType::Ack;
      r.ord_status = oe::OrdStatus::New;
      r.leaves_qty = req.qty;
      push(r);
      if (needed == 2) {
        r.exec_type = oe::ExecType::Expired;
        r.ord_status = oe::OrdStatus::Expired;
        r.leaves_qty = 0;
        push(r);
      }
      break;
    case oe::ReqType::Cancel:
      r.exec_type = oe::ExecType::Canceled;
      r.ord_status = oe::OrdStatus::Canceled;
      push(r);
      break;
    case oe::ReqType::Replace:
      r.exec_type = oe::ExecType::Replaced;
      r.ord_status = oe::OrdStatus::New;
      r.leaves_qty = req.qty; // exec subtracts cum from the new total
      push(r);
      break;
    case oe::ReqType::CancelAll:
      break; // exec cancels order by order in null mode
    }
    return true;
  }

  void on_frame(const MarketUpdate<EXEC_DEPTH> &) noexcept {}

  template <class F> void poll(F &&on_report) noexcept {
    while (head_ != tail_) {
      oe::ExecReport &r = q_[head_++ & MASK];
      r.tsc_recv = read_tsc();
      on_report(static_cast<const oe::ExecReport &>(r));
    }
  }

private:
  static constexpr uint32_t QUEUE = 1024;
  static constexpr uint32_t MASK = QUEUE - 1;
  void push(const oe::ExecReport &r) noexcept { q_[tail_++ & MASK] = r; }

  oe::ExecReport q_[QUEUE];
  uint32_t head_ = 0, tail_ = 0;
};
static_assert(Router<NullRouter>);

} // namespace exec

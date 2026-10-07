#pragma once

#include "common/ipc/shm.hpp"

namespace dashboard {

// Trading-side views over the exec and feed stats blocks in shm: the status
// strip (with the operator controls) and the Trading and Latency & Health tabs.
// Call update() once per UI frame, then the render functions.
class TradingPanels {
public:
  explicit TradingPanels(ipc::PipelineShm *p) noexcept;
  void update(double t_now) noexcept;
  void render_status_strip() noexcept;
  void render_trading_tab() noexcept;
  void render_health_tab() noexcept;

private:
  struct Series; // time series of a few floats, for ImPlot
  struct State;
  State *s_;
};

} // namespace dashboard

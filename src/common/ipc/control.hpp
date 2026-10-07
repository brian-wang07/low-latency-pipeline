#pragma once

#include <atomic>
#include <cstdint>

namespace ipc {

enum ControlBits : uint32_t {
  SHUTDOWN = 1,
  KILL = 2,  // cancel all, accept no new orders
  PAUSE = 4, // accept no new orders
  SWAP = 8,  // exec reloads its strategy module
};

// Operator controls, written by the manager and dashboard, polled by exec and
// the gateway.
struct alignas(64) Control {
  std::atomic<uint32_t> flags;
  uint32_t _pad;
  char swap_path[256 - 8];
};
static_assert(sizeof(Control) == 256);

} // namespace ipc

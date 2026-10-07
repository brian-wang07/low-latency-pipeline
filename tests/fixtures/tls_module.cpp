// A strategy module that uses thread_local: the loader must refuse it.
#include "exec/module_abi.hpp"

thread_local int tls_counter = 0;

namespace {
struct TlsStrategy {
  template <class Ctx> void on_tick(Ctx &) noexcept { ++tls_counter; }
};
} // namespace

LL_STRATEGY_MODULE(TlsStrategy, "tls_fixture")

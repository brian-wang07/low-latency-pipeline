// Pins the cross-process layout. A failure here means a wire type changed: if that
// was intended, bump ipc::VERSION and update the goldens below in the same change.
#include <cinttypes>
#include <cstddef>
#include <cstring>
#include <memory>

#include "check.hpp"
#include "common/ipc/layout.hpp"
#include "common/ipc/shm.hpp"

static constexpr uint32_t GOLDEN_VERSION = 3;
static constexpr uint32_t GOLDEN_LAYOUT_HASH = 0x1f31eb1a;
static constexpr std::size_t GOLDEN_SHM_BYTES = 6638208;

static void test_goldens() {
  std::printf("version=%u layout_hash=0x%08x sizeof(PipelineShm)=%zu\n",
              ipc::VERSION, ipc::LAYOUT_HASH, sizeof(ipc::PipelineShm));
  CHECK(ipc::VERSION == GOLDEN_VERSION);
  CHECK(ipc::LAYOUT_HASH == GOLDEN_LAYOUT_HASH);
  CHECK(sizeof(ipc::PipelineShm) == GOLDEN_SHM_BYTES);
}

static void test_wire_sizes() {
  using MU = exec::MarketUpdate<exec::EXEC_DEPTH>;
  CHECK(sizeof(MU) == 576);
  CHECK(offsetof(MU, best_bid) == 24);
  CHECK(offsetof(MU, instrument) == 56);
  CHECK(offsetof(MU, flags) == 59);
  CHECK(offsetof(MU, nb) == 60);
  CHECK(offsetof(MU, bids) == 72);
  CHECK(sizeof(oe::OrderRequest) == 64);
  CHECK(offsetof(oe::OrderRequest, price) == 32);
  CHECK(offsetof(oe::OrderRequest, instrument) == 48);
  CHECK(sizeof(oe::ExecReport) == 128);
  CHECK(offsetof(oe::ExecReport, venue_order_id) == 96);
  CHECK(sizeof(ref::Instrument) == 64);
  CHECK(offsetof(ref::Instrument, symbol) == 32);
  CHECK(offsetof(ref::Instrument, tick_exp) == 56);
  CHECK(sizeof(ipc::ShmHeader) == 64);
  CHECK(sizeof(ipc::Control) == 256);
  CHECK(offsetof(ipc::PipelineShm, control) == 64);
  CHECK(sizeof(ipc::PipelineShm) <= ipc::SHM_SIZE);
  CHECK(sizeof(ipc::PipelineShm) % 64 == 0);
}

static void test_instrument_table() {
  auto t = std::make_unique<ref::InstrumentTable>();
  CHECK(t->size() == 0);
  CHECK(t->find("NVDA", "ITCH") == ref::INVALID_INSTRUMENT);

  ref::Instrument a{};
  std::strcpy(a.symbol, "NVDA");
  std::strcpy(a.venue, "ITCH");
  a.tick_mant = 1;
  a.tick_exp = -4;
  ref::Instrument b{};
  std::strcpy(b.symbol, "BTC/USD");
  std::strcpy(b.venue, "KRAKEN");

  CHECK(t->add(a) == 0);
  CHECK(t->add(b) == 1);
  CHECK(t->size() == 2);
  CHECK(t->find("NVDA", "ITCH") == 0);
  CHECK(t->find("BTC/USD", "KRAKEN") == 1);
  CHECK(t->find("BTC/USD", "ITCH") == ref::INVALID_INSTRUMENT);
  CHECK(t->rows[0].tick_exp == -4);

  for (uint32_t i = 2; i < ref::MAX_INSTRUMENTS; ++i)
    CHECK(t->add(a) == i);
  CHECK(t->add(a) == ref::INVALID_INSTRUMENT);
  CHECK(t->size() == ref::MAX_INSTRUMENTS);
}

int main() {
  test_goldens();
  test_wire_sizes();
  test_instrument_table();
  return check_summary();
}

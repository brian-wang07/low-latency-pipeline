#include <cstring>
#include <memory>
#include <string>

#include "check.hpp"
#include "common/config_file.hpp"

static void test_basic() {
  auto c = std::make_unique<common::Config>();
  CHECK(c->parse("# comment\n"
                 "\n"
                 "  mode = itch  \n"
                 "max_events=20_000_000\n"
                 "risk.max_order_qty=500\r\n"
                 "price_band_bp = 25.5\n"
                 "dashboard=yes\n"
                 "feed_path=../itch_feed/S071321-v50.txt\n"));
  CHECK(c->size() == 6);
  CHECK(std::strcmp(c->get_str("mode", ""), "itch") == 0);
  CHECK(c->get_i64("max_events", 0) == 20'000'000);
  CHECK(c->get_i64("risk.max_order_qty", 0) == 500);
  CHECK(c->get_f64("price_band_bp", 0) == 25.5);
  CHECK(c->get_bool("dashboard", false));
  CHECK(std::strcmp(c->get_str("feed_path", ""), "../itch_feed/S071321-v50.txt") == 0);
  CHECK(std::strcmp(c->get_str("missing", "dflt"), "dflt") == 0);
  CHECK(c->get_i64("missing", -7) == -7);
  CHECK(!c->has("missing"));
}

static void test_duplicates_and_bad_values() {
  auto c = std::make_unique<common::Config>();
  CHECK(c->parse("a=1\nb=x12\nc=maybe\na=2\nd=1.5e\n"));
  CHECK(c->size() == 4);
  CHECK(c->get_i64("a", 0) == 2);       // last wins
  CHECK(c->get_i64("b", 42) == 42);     // not an integer
  CHECK(c->get_bool("c", true) == true); // not a bool
  CHECK(c->get_f64("d", 3.0) == 3.0);   // trailing junk
  CHECK(c->get_i64("a", 0) == 2);
}

static void test_malformed_lines() {
  auto c = std::make_unique<common::Config>();
  CHECK(!c->parse("ok=1\nnoequals\n=novalue\n"));
  CHECK(c->size() == 1);
  CHECK(c->get_i64("ok", 0) == 1);

  std::string long_val(common::Config::VAL_LEN, 'v');
  auto c2 = std::make_unique<common::Config>();
  CHECK(!c2->parse(("k=" + long_val + "\n").c_str()));
  CHECK(c2->size() == 0);
}

static void test_capacity() {
  auto c = std::make_unique<common::Config>();
  std::string text;
  for (int i = 0; i < common::Config::MAX_ENTRIES + 1; ++i)
    text += "k" + std::to_string(i) + "=" + std::to_string(i) + "\n";
  CHECK(!c->parse(text.c_str())); // the extra entry is rejected
  CHECK(c->size() == common::Config::MAX_ENTRIES);
  CHECK(c->get_i64("k255", -1) == 255);
  CHECK(c->get_i64("k256", -1) == -1);
}

static void test_prefix_and_unknown() {
  auto c = std::make_unique<common::Config>();
  CHECK(c->parse("null.balance.USD=100000\nnull.balance.NVDA=500\n"
                 "cores.exec=3\ntypo_key=1\n"));
  int n = 0;
  int64_t usd = 0;
  c->for_prefix("null.balance.", [&](const char *asset, const char *v) {
    ++n;
    if (std::strcmp(asset, "USD") == 0)
      usd = std::strtoll(v, nullptr, 10);
  });
  CHECK(n == 2);
  CHECK(usd == 100000);
  const char *known[] = {"null.balance.", "cores."};
  CHECK(c->warn_unknown(known, 2) == 1);
}

int main() {
  test_basic();
  test_duplicates_and_bad_values();
  test_malformed_lines();
  test_capacity();
  test_prefix_and_unknown();
  return check_summary();
}

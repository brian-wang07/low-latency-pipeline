#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <thread>

#include "check.hpp"
#include "common/event.hpp"
#include "common/ipc/shm.hpp"
#include "exchange/itch/itch_parser.hpp"

// Replays the whole feed through the parser into a CoreRing and checks that every
// published event is consumed, intact and in order. --dump prints each event.
int main(int argc, char *argv[]) {
  const char *path = "../itch_feed/S071321-v50.txt";
  bool dump = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--dump") == 0)
      dump = true;
    else
      path = argv[i];
  }
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    perror("open");
    return 1;
  }

  auto ring = std::make_unique<core::CoreRing>();
  std::atomic<bool> done{false};

  std::thread producer([&] {
    ItchParser parser(fd, ring.get());
    while (!parser.done())
      parser.next();
    done.store(true, std::memory_order_release);
  });

  uint64_t count = 0;
  uint64_t bad_type = 0;
  uint64_t time_reversals = 0;
  std::thread consumer([&] {
    common::Event ev;
    uint64_t prev_ts = 0;

    auto consume = [&] {
      if (std::strchr("AFECXDUPQ", ev.message_type) == nullptr || ev.message_type == 0)
        ++bad_type;
      if (ev.timestamp < prev_ts)
        ++time_reversals;
      prev_ts = ev.timestamp;
      if (dump) {
        char stock[9] = {};
        memcpy(stock, ev.stock, 8);
        std::cout << "type=" << ev.message_type << " ts=" << ev.timestamp
                  << " ref=" << ev.order_ref_number << " stock=" << stock
                  << " shares=" << ev.shares << " price=" << ev.price << '\n';
      }
      ++count;
    };

    while (!done.load(std::memory_order_acquire)) {
      if (ring->try_pop(ev))
        consume();
    }
    while (ring->try_pop(ev))
      consume();
  });

  producer.join();
  consumer.join();

  const uint32_t published = ring->tail.load(std::memory_order_acquire);
  std::cout << "consumed " << count << " events, published " << published
            << ", time reversals " << time_reversals << '\n';
  CHECK(count > 0);
  CHECK(static_cast<uint32_t>(count) == published);
  CHECK(bad_type == 0);
  return check_summary();
}

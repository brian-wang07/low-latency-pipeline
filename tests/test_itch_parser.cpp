#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <thread>

#include "common/event.hpp"
#include "common/ipc/exchange_shm.hpp"
#include "exchange/itch/itch_parser.hpp"

int main(int argc, char* argv[]) {
    const char* path = (argc > 1) ? argv[1] : "../itch_feed/S071321-v50.txt";
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    auto ring = std::make_unique<exchange::ExchangeRing>();
    std::atomic<bool> done{false};

    std::thread producer([&] {
        ItchParser parser(fd, ring.get());
        while (!parser.done())
            parser.next();
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        common::Event ev;
        int count = 0;

        auto print = [&] {
            char stock[9] = {};
            memcpy(stock, ev.stock, 8);
            std::cout
                << "type=" << ev.message_type
                << " ts="  << ev.timestamp
                << " ref=" << ev.order_ref_number
                << " stock=" << stock
                << " shares=" << ev.shares
                << " price=" << ev.price
                << '\n';
            ++count;
        };

        while (!done.load(std::memory_order_acquire)) {
            if (ring->try_pop(ev)) print();
        }
        while (ring->try_pop(ev)) print();

        std::cout << "consumed " << count << " events\n";
    });

    producer.join();
    consumer.join();
    return 0;
}

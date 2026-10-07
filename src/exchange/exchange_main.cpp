#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <thread>

#include "common/config.hpp"
#include "common/config_file.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
#include "exchange/itch/itch_parser.hpp"

static volatile std::sig_atomic_t shutdown_flag{0};
static void on_signal(int) { shutdown_flag = 1; }

int main(int argc, char **argv) {
  static common::Config cfg;
  if (argc < 3 || !cfg.load_from_args(argc, argv))
    return 2;

  int data_fd = open(argv[2], O_RDONLY);
  if (data_fd < 0) {
    std::perror("open");
    return 1;
  }

  ShmSegment shm;
  ipc::PipelineShm *p = ipc::attach_pipeline(argv[1], shm, "exchange_main");

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(int(cfg.get_i64("cores.exchange", config::EXCHANGE_CORE))))
    std::perror("pin_to_core exchange");

  ItchParser parser{data_fd, &p->exchange_to_core, &shutdown_flag};
  while (!parser.done() && !shutdown_flag) {
    parser.next();
    std::this_thread::sleep_for(std::chrono::nanoseconds(1));
  }
  return 0;
}
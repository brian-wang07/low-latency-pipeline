#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <string>

#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include "common/platform/cpu_pin.hpp"
#include "common/platform/spin_pause.hpp"
#include "exchange/itch/itch_parser.hpp"

static volatile std::sig_atomic_t shutdown_flag{0};
static void on_signal(int) { shutdown_flag = 1; }

int main(int argc, char **argv) {
  if (argc != 3)
    std::abort();

  int shm_fd = std::stoi(argv[1]);
  int data_fd = open(argv[2], O_RDONLY);
  if (data_fd < 0) {
    std::perror("open");
    return 1;
  }

  ShmSegment shm;
  if (!shm.attach(shm_fd, ipc::SHM_SIZE))
    std::abort();
  auto *p = shm.as<ipc::PipelineShm>();
  while (p->header.magic.load(std::memory_order_acquire) == 0) {
    SPIN_PAUSE();
  }
  if (p->header.magic != ipc::MAGIC)
    std::abort();

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  if (!pin_to_core(2))
    std::perror("pin_to_core exchange");

  ItchParser parser{data_fd, &p->exchange_to_core};
  while (!parser.done() && !shutdown_flag)
    parser.next();

  return 0;
}
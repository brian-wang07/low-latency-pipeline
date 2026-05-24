#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <new>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile std::sig_atomic_t shutdown{0};

static void on_signal(int) { shutdown = 1; }

struct WorkerSpec {
  const char *path;
  const char *extra_arg; // nullptr if the worker takes only the shm fd
};

static constexpr WorkerSpec WORKERS[] = {
    {"./exchange_main", "../itch_feed/S071321-v50.txt"},
    {"./core_main", nullptr},
};

static pid_t spawn(const WorkerSpec &w, int shm_fd, pid_t parent_pid) {
  pid_t pid = fork();
  if (pid < 0) {
    std::perror("fork");
    return -1;
  }
  if (pid > 0)
    return pid;

  // chilD
  int flags = fcntl(shm_fd, F_GETFD);
  if (flags != -1)
    fcntl(shm_fd, F_SETFD, flags & ~FD_CLOEXEC);
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() != parent_pid)
    _exit(1);

  char fd_str[16];
  std::snprintf(fd_str, sizeof(fd_str), "%d", shm_fd);

  char *argv[4] = {const_cast<char *>(w.path), fd_str, nullptr, nullptr};
  if (w.extra_arg)
    argv[2] = const_cast<char *>(w.extra_arg);

  execv(w.path, argv);
  std::perror("execv");
  _exit(127);
}

int main() {
  ShmSegment shm;
  if (!shm.create(ipc::SHM_NAME, ipc::SHM_SIZE))
    std::abort();

  auto *p = new (shm.get_address()) ipc::PipelineShm();

  p->header.version = ipc::VERSION;
  p->header.magic.store(ipc::MAGIC, std::memory_order_release);

  pid_t pgid = getpgrp();

  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  pid_t parent_pid = getpid();
  std::size_t alive = 0;
  for (const auto &w : WORKERS) {
    if (spawn(w, shm.fd(), parent_pid) > 0)
      ++alive;
  }

  int exit_code = 0;
  while (alive > 0) {
    int status;
    pid_t r = waitpid(-1, &status, 0);
    if (r == -1) {
      if (errno == EINTR) {
        if (shutdown)
          kill(-pgid, SIGTERM);
        continue;
      }
      std::perror("waitpid");
      break;
    }
    --alive;
    if (!shutdown) {
      shutdown = 1;
      exit_code = 1;
      kill(-pgid, SIGTERM);
    }
  }

  return exit_code;
}

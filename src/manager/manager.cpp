// Creates the shm segment, launches the workers named by a pipeline config and
// supervises them:
//
//   manager [pipeline.cfg]        (default ../configs/itch-null.cfg; run from build/)
//
//   - feed producer (exchange_main, core_main) exits 0: the feed is done or
//     max_events was reached -> graceful shutdown of everything.
//   - feed producer dies: KILL|PAUSE, give exec a moment to see it, shut down.
//   - exec exits 75 (Control.SWAP) or the manager gets SIGUSR1: restart exec alone,
//     with Control.swap_path if set, else the config's strategy (re-read).
//   - exec dies unexpectedly: set KILL (stays set until the operator clears it);
//     respawn if auto_restart_exec=1, else shut down.
//   - dashboard exits: ignored.
#include "common/config_file.hpp"
#include "common/ipc/layout.hpp"
#include "common/ipc/shm.hpp"
#include "common/ipc/shm_segment.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static volatile std::sig_atomic_t shutdown_req{0};
static volatile std::sig_atomic_t restart_exec_req{0};
static void on_signal(int) { shutdown_req = 1; }
static void on_usr1(int) { restart_exec_req = 1; }

static constexpr int EXIT_SWAP = 75; // exec asks to be respawned

enum Role { EXCHANGE, CORE, EXEC, DASHBOARD, ROLES };
static const char *const ROLE_NAMES[ROLES] = {"exchange_main", "core_main", "exec",
                                              "dashboard"};

struct Worker {
  pid_t pid = -1;
  char path[256];
  char args[4][300]; // after the shm fd
  int nargs = 0;
};

static pid_t spawn(const Worker &w, int shm_fd, pid_t parent_pid) {
  pid_t pid = fork();
  if (pid < 0) {
    std::perror("fork");
    return -1;
  }
  if (pid > 0)
    return pid;

  // child
  int flags = fcntl(shm_fd, F_GETFD);
  if (flags != -1)
    fcntl(shm_fd, F_SETFD, flags & ~FD_CLOEXEC);
  prctl(PR_SET_PDEATHSIG, SIGKILL);
  if (getppid() != parent_pid)
    _exit(1);

  char fd_str[16];
  std::snprintf(fd_str, sizeof(fd_str), "%d", shm_fd);
  char *argv[7] = {const_cast<char *>(w.path), fd_str};
  for (int i = 0; i < w.nargs; ++i)
    argv[2 + i] = const_cast<char *>(w.args[i]);
  argv[2 + w.nargs] = nullptr;
  execv(w.path, argv);
  std::perror("execv");
  _exit(127);
}

static void add_arg(Worker &w, const char *fmt, const char *v) {
  std::snprintf(w.args[w.nargs++], sizeof(w.args[0]), fmt, v);
}

// exec binary for a strategy spec: builtin:<name> -> ./exec_main_<name>; a module
// path -> the generic ./exec_main loading it.
static void make_exec(Worker &w, const char *spec, const char *mode,
                      const char *cfg_path) {
  w.nargs = 0;
  const char *name = std::strncmp(spec, "builtin:", 8) == 0 ? spec + 8 : nullptr;
  if (name && !std::strchr(name, '/'))
    std::snprintf(w.path, sizeof(w.path), "./exec_main_%s", name);
  else
    std::snprintf(w.path, sizeof(w.path), "./exec_main");
  add_arg(w, "--mode=%s", mode);
  add_arg(w, "--strategy=%s", spec);
  add_arg(w, "--config=%s", cfg_path);
}

static const char *const KNOWN_KEYS[] = {
    "mode",      "feed_path",       "symbols",           "max_events",
    "dashboard", "strategy",        "strategy.params",   "auto_restart_exec",
    "exec.",     "cores.",          "risk.",             "rate.",
    "kraken.",   "coinbase.",       "null.balance.",     "live",
};

int main(int argc, char **argv) {
  const char *cfg_path = argc > 1 ? argv[1] : "../configs/itch-null.cfg";
  static common::Config cfg;
  if (!cfg.load(cfg_path))
    return 2;
  cfg.warn_unknown(KNOWN_KEYS, int(sizeof(KNOWN_KEYS) / sizeof(KNOWN_KEYS[0])));
  if (std::strcmp(cfg.get_str("mode", "itch"), "itch") != 0) {
    std::fprintf(stderr, "manager: mode=%s is not available yet (plan Phase 4)\n",
                 cfg.get_str("mode", ""));
    return 2;
  }

  ShmSegment shm;
  if (!shm.create(ipc::SHM_NAME, ipc::SHM_SIZE))
    return 1;
  auto *p = new (shm.get_address()) ipc::PipelineShm();
  p->header.version = ipc::VERSION;
  p->header.layout_hash = ipc::LAYOUT_HASH;
  p->header.magic.store(ipc::MAGIC, std::memory_order_release);

  pid_t pgid = getpgrp();
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sa.sa_handler = on_usr1;
  sigaction(SIGUSR1, &sa, nullptr);

  static Worker w[ROLES];
  std::snprintf(w[EXCHANGE].path, sizeof(w[0].path), "./exchange_main");
  add_arg(w[EXCHANGE], "%s", cfg.get_str("feed_path", "../itch_feed/S071321-v50.txt"));
  add_arg(w[EXCHANGE], "--config=%s", cfg_path);
  std::snprintf(w[CORE].path, sizeof(w[0].path), "./core_main");
  add_arg(w[CORE], "--config=%s", cfg_path);
  const char *exec_mode = cfg.get_str("exec.mode", "null");
  make_exec(w[EXEC], cfg.get_str("strategy", "builtin:logging"), exec_mode, cfg_path);
  std::snprintf(w[DASHBOARD].path, sizeof(w[0].path), "./dashboard");
  add_arg(w[DASHBOARD], "--config=%s", cfg_path);
  const bool want_dashboard = cfg.get_bool("dashboard", true);
  const bool auto_restart = cfg.get_bool("auto_restart_exec", false);

  const pid_t self = getpid();
  for (int r = 0; r < ROLES; ++r)
    if (r != DASHBOARD || want_dashboard)
      w[r].pid = spawn(w[r], shm.fd(), self);

  auto set_control = [&](uint32_t bits) {
    p->control.flags.fetch_or(bits, std::memory_order_acq_rel);
  };
  // Next exec strategy: Control.swap_path if set (then cleared), else the config's.
  auto next_strategy = [&](char *out, size_t n) {
    if (p->control.swap_path[0]) {
      std::snprintf(out, n, "%.*s", int(sizeof(p->control.swap_path)),
                    p->control.swap_path);
      std::memset(p->control.swap_path, 0, sizeof(p->control.swap_path));
      return;
    }
    static common::Config fresh;
    fresh = common::Config{};
    const bool ok = fresh.load(cfg_path);
    std::snprintf(out, n, "%s",
                  ok ? fresh.get_str("strategy", "builtin:logging")
                     : cfg.get_str("strategy", "builtin:logging"));
  };
  auto respawn_exec = [&](const char *why) {
    char spec[256];
    next_strategy(spec, sizeof(spec));
    make_exec(w[EXEC], spec, exec_mode, cfg_path);
    std::fprintf(stderr, "[manager] %s: starting %s --strategy=%s\n", why,
                 w[EXEC].path, spec);
    w[EXEC].pid = spawn(w[EXEC], shm.fd(), self);
  };

  int exit_code = 0;
  bool shutting_down = false;
  bool exec_restart_pending = false;
  auto begin_shutdown = [&](int code) {
    if (shutting_down)
      return;
    shutting_down = true;
    exit_code = code;
    kill(-pgid, SIGTERM);
  };

  for (;;) {
    if (shutdown_req && !shutting_down)
      begin_shutdown(0);
    if (restart_exec_req) {
      restart_exec_req = 0;
      if (!shutting_down && w[EXEC].pid > 0 && !exec_restart_pending) {
        exec_restart_pending = true;
        kill(w[EXEC].pid, SIGTERM);
      }
    }

    int status;
    const pid_t r = waitpid(-1, &status, WNOHANG);
    if (r == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break; // ECHILD: everyone has exited
    }

    int role = -1;
    for (int k = 0; k < ROLES; ++k)
      if (w[k].pid == r)
        role = k;
    if (role < 0)
      continue;
    w[role].pid = -1;
    const bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (shutting_down)
      continue;

    switch (role) {
    case EXCHANGE:
    case CORE:
      if (clean) {
        std::fprintf(stderr, "[manager] %s finished; shutting down\n", ROLE_NAMES[role]);
        begin_shutdown(0);
      } else {
        std::fprintf(stderr, "[manager] %s died (status %d); KILL+PAUSE\n",
                     ROLE_NAMES[role], status);
        set_control(ipc::KILL | ipc::PAUSE);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        begin_shutdown(1);
      }
      break;
    case EXEC:
      if (exec_restart_pending) {
        exec_restart_pending = false;
        respawn_exec("SIGUSR1");
      } else if (WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SWAP) {
        respawn_exec("swap requested");
      } else {
        std::fprintf(stderr, "[manager] exec died (status %d); KILL set\n", status);
        set_control(ipc::KILL);
        if (auto_restart)
          respawn_exec("auto_restart_exec");
        else
          begin_shutdown(1);
      }
      break;
    case DASHBOARD:
      std::fprintf(stderr, "[manager] dashboard exited; pipeline continues\n");
      break;
    }
  }
  return exit_code;
}

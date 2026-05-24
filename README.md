
## architecture

```
┌────────────────────────────────────────────────┐                                                                   
│              EXCHANGE (Process 1)              │                                                                   
│                                                │                                                                   
│     [CREATOR]───────HEAP────────►[DISPATCHER]──┼─────────┐ 
│                                                │         │   
│   [ADVERSARIES]────────────────────────────────┼──────┐  │  give each producer its own spsc ring,                       
│                                                │     ┌▼──▼┐ and k-way merge on the enqueue timestamp.
│ [MATCHING ENGINE]◄─────────────────────────────┼─────┤SPSC◄─────┐                                                  
│         │                                      │     └────┘     │                                                  
└─────────┼──────────────────────────────────────┘    EXCHANGE    │                                                  
       ┌──▼─┐                                                     │                                                  
       │SPSC│ENGINE                                               │                                                  
       └──┬─┘                                                     │                                                  
┌─────────┼──────────────────────────────────────┐                │                                                  
│         │ RUNTIME ENGINE (Process 2)           │                │                                                  
│         │                                      │ DASHBOARD      │                                                  
│         ▼         ┌───────┐                    │ ┌────┐         │                                                  
│    [HOT PATH]─────►Seqlock┼────►[SNAPSHOTTER]──┼─►SPSC│         │                                                  
│         │         └───────┘                    │ └──┬─┘         │                                                  
└─────────┼──────────────────────────────────────┘    │           │                                                  
          │                                           │           │                                                  
       ┌──▼─┐                      ┌──────────────────┴──┐        │                                                  
       │SPSC│STRATEGY              │DASHBOARD (Process 4)│        │                                                  
       └──┬─┘                      └─────────────────────┘        │                                                  
          │                                                       │                                                  
┌─────────┼──────────────────────────────────────┐                │                                                  
│         │    STRATEGY (Process 3)              │                │                                                  
│         ▼   Dispatch to main or offload        │                │                                                  
│   [DISPATCHER]───────────────────────►[MAIN]   │                │                                                  
│         │                                │     │                │                                                  
│         ▼                                │     │                │                                                  
│   [ACCELERATOR]──────────────────────────┴─────┼────────────────┘                                                  
│                                                │                                                                   
└────────────────────────────────────────────────┘                                                                   
```

Explicit Hugepages setup:
Manager is responsible for raii of page, thuis we use memfd_create. 
1. Allocate hugepages
```
sudo sysctl -w vm.nr_hugepages=20
```
verify: 
```
grep Huge /proc/meminfo
```
`HugePages_Total` should be set to 20.

To persist over reboots, add the following in `/etc/sysctl.conf` (edit with sudo permissions):
```
vm.nr_hugepages=20
```
2. By default, only processes with root level access can access the hugepages. Either start the manager process with sudo, or run the following:
```
sudo sysctl -w vm.hugetlb_shm_group=1000
```
To persist over reboots, add the following in `etc/sysctl.conf`:
```
vm.hugetlb_shm_group=1000
```

## running the pipeline

```
cd build
./manager
```

The manager owns the lifetime of everything else. On startup it:

1. Creates a memfd-backed shared segment from the hugepage pool reserved above.
2. Placement-news the on-shm structures and release-stores a magic number so workers know the layout is fully published before they read it.
3. Forks + execs `exchange_main` (ITCH parser → ring) and `core_main` (ring → orderbook → renderer), passing the inherited shm fd as `argv[1]`. The shm fd is kept inheritable (`FD_CLOEXEC` cleared) so it survives `execve`.
4. Each child calls `prctl(PR_SET_PDEATHSIG, SIGKILL)` between fork and exec — if the manager dies, the kernel kills the children immediately. No orphans, no stale state.
5. Sits in `waitpid` for the lifetime of the run.

### shutdown

`Ctrl-C` (SIGINT) or SIGTERM to the manager flips a `sig_atomic_t` flag inside the handler. The signal also interrupts `waitpid` with `EINTR`, at which point the manager does `kill(-pgid, SIGTERM)` — one syscall, broadcast to the entire process group, reaches every worker at once. Workers' own handlers flip their shutdown flags, their main loops exit, they drain any in-flight events from the ring, and `return`. The manager reaps each via `waitpid` until none remain, then returns.

If any worker dies unprompted (crash, or `exchange_main` reaching end-of-feed), the manager treats it the same: broadcasts SIGTERM to the rest, drains, exits nonzero so the shell knows it wasn't clean.

Cleanup of the shared segment is automatic — when the last `ShmSegment` dtor closes the last fd referencing the memfd, the kernel reclaims the pages. No `shm_unlink`, no leaked names across runs.

## cpu pinning

Each worker pins its main thread to a specific core via `pin_to_core(N)` (in `src/common/platform/cpu_pin.hpp`, wraps `pthread_setaffinity_np`):

| process         | core    |
|-----------------|---------|
| `exchange_main` | 2       |
| `core_main`     | 4       |
| `manager`       | unpinned (sleeps in `waitpid`, no hot path) |

Chosen so that (a) neither worker shares an SMT sibling pair with the other, (b) cores 0 and 1 stay free since the kernel typically routes interrupts there. Verify the sibling layout on your machine:

```
cat /sys/devices/system/cpu/cpu2/topology/thread_siblings_list
```

### isolating the cores from the scheduler

Pinning only says "I prefer this core" — the scheduler can still place other tasks on it. For tighter latency, tell the kernel to leave those cores alone entirely. Edit `/etc/default/grub`:

```
GRUB_CMDLINE_LINUX="... isolcpus=2,4 nohz_full=2,4 rcu_nocbs=2,4"
```

Then regenerate grub and reboot:

```
sudo grub2-mkconfig -o /boot/grub2/grub.cfg
sudo reboot
```

- `isolcpus=2,4` — scheduler stops placing tasks on these cores. Only processes that explicitly pin themselves there (i.e. our workers) run on them.
- `nohz_full=2,4` — kills the periodic scheduler tick on those cores while a single task is running. Eliminates ~250 Hz of timer-induced jitter per core.
- `rcu_nocbs=2,4` — offloads RCU callbacks to other CPUs so our hot path never gets preempted by RCU bookkeeping.

## verification

While the pipeline is running:

```
ps -eLo pid,tid,psr,comm | grep -E 'exchange_main|core_main'
```

The `psr` column is the CPU each thread last ran on. With pinning in effect, `exchange_main` shows `2` and `core_main` shows `4`. Without `isolcpus` they can still be scheduled there even when the kernel runs other tasks alongside them; with `isolcpus` they're the only userspace on those cores.

For a live view, `btop` (or `htop`) shows per-core utilization. With the pipeline running you should see cores 2 and 4 pegged near 100% (both workers busy-poll their ring) and the rest of the cores essentially idle. If you see the load wandering across cores instead, pinning didn't take — re-check whether `pin_to_core` returned true (it `perror`s on failure but doesn't abort).

Confirm the shared segment exists and is hugetlb-backed while running:

```
ls -l /proc/$(pgrep manager)/fd | grep memfd
grep -A1 HugetlbPages /proc/$(pgrep manager)/status
```

The first should show one memfd entry; the second should show non-zero `HugetlbPages` matching the segment size.

For a baseline wake-up jitter measurement (before vs after `isolcpus`/`nohz_full`):

```
sudo cyclictest -m -p 99 -t1 -a 2 -i 200 -l 100000
```

Pins to core 2, runs 100 000 iterations at 200 µs nominal period, prints max/avg latency.


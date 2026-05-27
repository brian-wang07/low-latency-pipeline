
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

## orderbook

The engine in `src/core/` reconstructs a per-symbol limit order book from the ITCH stream. Going from leaves to root:

**`OrderEntry`** — `{price, shares, side}` per live order. ITCH's cancel/execute/delete reference an order by `OrderRef` and do *not* repeat side or price, so we have to remember them ourselves.

**`OrderMap<Capacity>`** (`OrderRef → OrderEntry`) — flat, open-addressed hash with robin-hood eviction. Chosen because:
- Flat slot array: zero heap allocs on insert, no pointer chasing on lookup.
- Linear probing: probes walk consecutive slots → cache-friendly.
- Robin-hood + shift-back erase: bounds worst-case probe length under load and avoids the tombstone tax of lazy deletion.
- Power-of-two capacity (`262144`): mask replaces modulo.
- Golden-ratio multiplicative hash: ITCH refs are near-sequential and would collide badly with identity hashing.
- No alignment padding: single-writer, no concurrent readers on this structure (readers go through the snapshot).

**`PriceLevel`** — `{total_shares, order_count}` aggregate per price. Pre-aggregated so reads never traverse a per-price order list. `order_count` is kept separately because we need to detect the moment a level drains to trigger rescan.

**`PriceLevelArray<MaxLevels>`** — flat array of `PriceLevel`, indexed by `(price - base_price) / PRICE_TICK`. One per side. Chosen because:
- O(1) random-access by price: one subtract-divide-load, vs O(log n) and pointer-chasing for `std::map`.
- Zero allocation, no rebalancing.
- Sequential memory layout makes `top_bids` / `top_asks` walks prefetcher-friendly.
- Bucketed by cent (`PRICE_TICK = 100`) since Reg NMS forbids sub-penny pricing for stocks ≥ $1 — costs nothing for normal equities and gives 100× more level range for the same memory. Window is $2621.44 wide.

The trade-off is the fixed window. `base_price` is seeded from the first add for a symbol minus a small offset; out-of-range adds are silently dropped (and currently logged to stdout while we tune).

**`TopOfBook`** — `{best_bid, best_ask}` cache. Without it, every TOB query would scan the level array. Maintained incrementally: `on_add` updates with a single compare-store, `rescan` only runs when a level drains. Sentinel values (`0` and `UINT32_MAX`) naturally lose the first comparison so the first real order always wins.

**`OrderBook`** — composes `TopOfBook` + two `PriceLevelArray`s + one `OrderMap` for a single symbol. ~10 MB. Non-copyable.

**`BookArray`** — `unique_ptr<OrderBook>[65536]` indexed by ITCH `stock_locate` (the 16-bit per-symbol id assigned at SoD). Direct array index = O(1) symbol lookup at the very top of the hot path, no hashing or string compare. `unique_ptr` so the 10 MB per book only allocates for symbols actually seen.

**`BookSnapshot<depth>` + `BookSeqlock<depth>`** — the publish boundary. Snapshot is a POD frame (TOB + top N levels per side). The seqlock ships it lock-free from the writer thread to the snapshotter; readers retry on torn reads. This decouples the single-writer hot path from any reader's pace.

Overall shape: **hash for "find this order"**, **two arrays for "find this price level"**, **cached pair for "find best price"**, **outer array for "find this symbol"**, **seqlock + POD frame for "ship state out"**. Direct indexing where the key space is small and dense, hashing where it's large and sparse.

## hugepages

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


#pragma once

namespace config {

// CPU pinning. Coordinated with kernel boot params:
//   isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5
//   irqaffinity=0,1,6,7,8,9,10,11
//
// EXCHANGE_CORE, HOT_CORE, and EXEC_CORE busy-poll their rings, so all three must be
// on isolated CPUs. SNAPSHOT_CORE should be on a different *physical* core than
// HOT_CORE (not its HT sibling) to avoid L1/L2/execution-port contention.
// LAT_DUMP_CORE belongs on a non-isolated CPU since it does file I/O.
//
// Current topology (lscpu -e=CPU,CORE):
//   Core 0: CPUs 0,1   Core 1: CPUs 2,3
//   Core 2: CPUs 4,5   Core 3: CPUs 6,7
//   Core 4: CPUs 8,9   Core 5: CPUs 10,11
//
// Only CPUs 2-5 (physical cores 1 and 2) are isolated, which isn't enough for three
// busy-pollers to each own a physical core. EXEC_CORE (3) is therefore the HT sibling
// of EXCHANGE_CORE (2): this keeps the headline hot path (HOT_CORE, physical core 2)
// uncontended, trading exchange<->exec contention on physical core 1 instead. For
// contention-free runs, isolate a third physical core (e.g. add 6,7 to isolcpus and
// drop them from irqaffinity) and move EXEC_CORE onto it.
inline constexpr int EXCHANGE_CORE = 2;
inline constexpr int HOT_CORE = 4;
inline constexpr int EXEC_CORE = 3;
inline constexpr int SNAPSHOT_CORE = 6;
inline constexpr int LAT_DUMP_CORE = 0;

} // namespace config

#pragma once

namespace config {

// CPU pinning. Coordinated with kernel boot params:
//   isolcpus=2,3,4,5 nohz_full=2,3,4,5 rcu_nocbs=2,3,4,5
//   irqaffinity=0,1,6,7,8,9,10,11
//
// EXCHANGE_CORE and HOT_CORE must be on isolated CPUs.
// SNAPSHOT_CORE should be on a different *physical* core than HOT_CORE
// (not its HT sibling) to avoid L1/L2/execution-port contention.
// LAT_DUMP_CORE belongs on a non-isolated CPU since it does file I/O.
//
// Current topology (lscpu -e=CPU,CORE):
//   Core 0: CPUs 0,1   Core 1: CPUs 2,3
//   Core 2: CPUs 4,5   Core 3: CPUs 6,7
//   Core 4: CPUs 8,9   Core 5: CPUs 10,11
inline constexpr int EXCHANGE_CORE = 2;
inline constexpr int HOT_CORE = 4;
inline constexpr int SNAPSHOT_CORE = 6;
inline constexpr int LAT_DUMP_CORE = 0;

} // namespace config

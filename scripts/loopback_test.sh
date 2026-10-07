#!/usr/bin/env bash
# End-to-end check of the ITCH pipeline with the NullRouter (plan "Verification").
# For each strategy it runs the manager on configs/itch-null.cfg (max_events, no
# dashboard) and checks strategy_stats.log; the dummy module run must match the
# baked exec_main_dummy order for order. Run from anywhere; uses build/.
#
#   scripts/loopback_test.sh [extra cfg lines...]
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root/build" || exit 1
out=$(mktemp -d)
failures=0

check() { # description, condition result (0 = pass)
  if [[ $2 -eq 0 ]]; then echo "  ok   $1"; else echo "  FAIL $1"; failures=$((failures + 1)); fi
}
val() { grep -m1 "^$2=" "$out/$1.stats" | cut -d= -f2; }

run() { # name strategy-spec
  local name=$1 spec=$2 cfg="$out/$1.cfg"
  cp ../configs/itch-null.cfg "$cfg"
  printf 'strategy=%s\ndashboard=0\nexec.stats_log=%s\n' "$spec" "$out/$name.stats" >>"$cfg"
  for extra in "${@:3}"; do echo "$extra" >>"$cfg"; done
  echo "== $name ($spec)"
  # The manager signals its whole process group on shutdown: give it its own.
  LAT_LOG="$out/$name.lat" setsid -w ./manager "$cfg" >"$out/$name.out" 2>&1
  local rc=$?
  check "manager exit 0 (got $rc)" $((rc != 0))
  if [[ ! -s "$out/$name.stats" ]]; then
    check "strategy_stats.log written" 1
    return
  fi
  local orders acks replaces replaced p99
  orders=$(val "$name" orders_new); acks=$(val "$name" acks)
  replaces=$(val "$name" orders_replace); replaced=$(val "$name" replaced)
  check "frame_gaps == 0 ($(val "$name" frame_gaps))" $(( $(val "$name" frame_gaps) != 0 ))
  check "orders sent > 0 ($orders)" $(( orders == 0 ))
  check "acks == orders sent ($acks)" $(( acks != orders ))
  check "replaced == replaces sent ($replaced)" $(( replaced != replaces ))
  check "fills == 0" $(( $(val "$name" fills) != 0 ))
  check "open orders at exit == 0" $(( $(val "$name" open_orders_at_exit) != 0 ))
  check "ring_full_rejects == 0" $(( $(val "$name" ring_full_rejects) != 0 ))
  # Histogram buckets are powers of two, and a p99 needs enough samples to mean
  # anything: with fewer than 1000 orders it is just the slowest one or two.
  local n p50 max
  n=$(val "$name" tick_to_order_n); p99=$(val "$name" tick_to_order_p99_ns)
  p50=$(val "$name" tick_to_order_p50_ns); max=$(val "$name" tick_to_order_max_ns)
  if (( n >= 1000 )); then
    check "tick_to_order p99 < 2us (bucket ${p99}ns, n=$n)" $(( p99 >= 2000 ))
  else
    check "tick_to_order p50 < 2us (bucket ${p50}ns; n=$n too few for p99, max ${max}ns)" \
      $(( p50 >= 2000 ))
  fi
}

run dummy builtin:dummy "$@"
check "dummy hits risk rejects ($(val dummy risk_rejects))" $(( $(val dummy risk_rejects) == 0 ))
run imbalance_taker builtin:imbalance_taker "$@"
run dummy_so ./strategy_dummy.so "$@"
for k in orders_new orders_replace orders_cancel risk_rejects; do
  check "dummy .so $k == baked ($(val dummy_so $k) vs $(val dummy $k))" \
    $(( $(val dummy_so $k) != $(val dummy $k) ))
done

echo "logs in $out"
if [[ $failures -ne 0 ]]; then
  echo "loopback: $failures check(s) failed"
  exit 1
fi
echo "loopback: all checks passed"

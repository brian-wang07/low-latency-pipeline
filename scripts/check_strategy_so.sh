#!/usr/bin/env bash
# Post-build gate for strategy modules: fails the build if a module could slow the
# hot path or outlive a swap badly. Checks:
#   - no PT_TLS segment (thread_local -> __tls_get_addr on every access)
#   - no static or thread_local destructors (__cxa_atexit / __cxa_thread_atexit)
#   - the only exported symbol is ll_strategy_module
#   - undefined symbols resolve only to glibc, libm, libstdc++ or libgcc
set -euo pipefail
so="$1"
fail() { echo "check_strategy_so: $so: $*" >&2; exit 1; }

if readelf -lW "$so" | awk '{print $1}' | grep -qx TLS; then
  fail "has a PT_TLS segment (thread_local in a module)"
fi

undef=$(nm -D --undefined-only "$so" | awk '{print $2}')
if grep -qE '^__cxa_(thread_)?atexit' <<<"$undef"; then
  fail "registers static or thread_local destructors (__cxa_atexit)"
fi
bad_undef=$(grep -vE '@(GLIBC|GLIBCXX|CXXABI|GCC)_|^(_ITM_|__gmon_start__|__cxa_finalize)' <<<"$undef" || true)
if [[ -n "$bad_undef" ]]; then
  fail "undefined symbols outside libc/libm/libstdc++/libgcc: $(tr '\n' ' ' <<<"$bad_undef")"
fi

exports=$(nm -D --defined-only "$so" | awk '{print $3}' | grep -v '^$' || true)
if [[ "$exports" != "ll_strategy_module" ]]; then
  fail "exports more than ll_strategy_module: $(tr '\n' ' ' <<<"$exports")"
fi

#pragma once

#include "common/ipc/layout.hpp"
#include "exec/exec_context.hpp"
#include "exec/router.hpp"
#include "exec/runner.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <type_traits>

// The only boundary between exec_main and a strategy module. A module compiles
// the whole hot loop (run_strategy<Type, Router>) itself, so on_tick inlines
// exactly as in a baked exec_main_<name>; exec_main only calls run_*() once.

#ifndef LL_BUILD_ID
#define LL_BUILD_ID "unknown"
#endif

namespace exec {

inline constexpr uint32_t MODULE_ABI_VERSION = 1;

// In-process types a module shares with exec_main. Size and alignment only: these
// are not standard-layout, and module and host are built by the same toolchain.
constexpr uint32_t exec_abi_hash() noexcept {
  const uint64_t parts[] = {
      sizeof(ExecContext), alignof(ExecContext), sizeof(MarketView),
      sizeof(Account),     sizeof(OrderTable),   sizeof(OrderRow),
      sizeof(RiskGate),    sizeof(RateModel),    sizeof(NullRouter),
      MODULE_ABI_VERSION,
  };
  uint32_t h = ipc::LAYOUT_HASH;
  for (uint64_t p : parts)
    h = ipc::detail::fnv1a(h, p);
  return h;
}

// ISA extensions the module was compiled to use (-march=native makes modules
// machine-specific); the loader refuses one the CPU lacks instead of SIGILLing.
enum IsaBits : uint32_t {
  ISA_SSE42 = 1u << 0,
  ISA_POPCNT = 1u << 1,
  ISA_AVX = 1u << 2,
  ISA_AVX2 = 1u << 3,
  ISA_BMI2 = 1u << 4,
  ISA_FMA = 1u << 5,
  ISA_AVX512F = 1u << 6,
};

constexpr uint32_t build_isa_mask() noexcept {
  uint32_t m = 0;
#ifdef __SSE4_2__
  m |= ISA_SSE42;
#endif
#ifdef __POPCNT__
  m |= ISA_POPCNT;
#endif
#ifdef __AVX__
  m |= ISA_AVX;
#endif
#ifdef __AVX2__
  m |= ISA_AVX2;
#endif
#ifdef __BMI2__
  m |= ISA_BMI2;
#endif
#ifdef __FMA__
  m |= ISA_FMA;
#endif
#ifdef __AVX512F__
  m |= ISA_AVX512F;
#endif
  return m;
}

inline uint32_t cpu_isa_mask() noexcept {
  __builtin_cpu_init();
  uint32_t m = 0;
  m |= __builtin_cpu_supports("sse4.2") ? uint32_t(ISA_SSE42) : 0u;
  m |= __builtin_cpu_supports("popcnt") ? uint32_t(ISA_POPCNT) : 0u;
  m |= __builtin_cpu_supports("avx") ? uint32_t(ISA_AVX) : 0u;
  m |= __builtin_cpu_supports("avx2") ? uint32_t(ISA_AVX2) : 0u;
  m |= __builtin_cpu_supports("bmi2") ? uint32_t(ISA_BMI2) : 0u;
  m |= __builtin_cpu_supports("fma") ? uint32_t(ISA_FMA) : 0u;
  m |= __builtin_cpu_supports("avx512f") ? uint32_t(ISA_AVX512F) : 0u;
  return m;
}

} // namespace exec

extern "C" {
struct ll_strategy_module_v1 {
  uint32_t abi_version; // MODULE_ABI_VERSION
  uint32_t layout_hash; // exec_abi_hash()
  uint32_t isa_mask;    // build_isa_mask()
  uint32_t _pad;
  const char *name;
  const char *build_id;
  void *(*create)(exec::ExecContext *, const char *params);
  // Runs on_start, the hot loop, then on_stop.
  exec::RunExit (*run_null)(void *, exec::ExecContext *, exec::NullRouter *);
  exec::RunExit (*run_live)(void *, exec::ExecContext *, void *router); // Phase 4
  // Strategy-private state for an in-process swap: a versioned header plus the
  // strategy object if it is trivially copyable; import accepts only its own type.
  size_t (*export_state)(void *, void *buf, size_t cap);
  bool (*import_state)(void *, const void *buf, size_t len);
  void (*destroy)(void *);
};
}

namespace exec::module_detail {

inline constexpr uint32_t STATE_MAGIC = 0x4c4c5354; // "LLST"

struct StateHeader {
  uint32_t magic, type_hash;
  uint64_t size;
};

constexpr uint32_t name_hash(const char *s) noexcept {
  uint32_t h = 2166136261u;
  while (*s)
    h = (h ^ uint8_t(*s++)) * 16777619u;
  return h;
}

template <class S, const char *Name> struct Impl {
  static void *create(ExecContext *, const char *) noexcept {
    return new (std::nothrow) S();
  }
  static RunExit run_null(void *p, ExecContext *x, NullRouter *r) noexcept {
    S &s = *static_cast<S *>(p);
    StrategyContext<NullRouter> ctx(*x, *r);
    call_on_start(s, ctx);
    const RunExit e = run_strategy(s, *x, *r);
    call_on_stop(s, ctx);
    return e;
  }
  static size_t export_state(void *p, void *buf, size_t cap) noexcept {
    if constexpr (std::is_trivially_copyable_v<S>) {
      const size_t need = sizeof(StateHeader) + sizeof(S);
      if (cap < need)
        return 0;
      const StateHeader h{STATE_MAGIC, name_hash(Name), sizeof(S)};
      std::memcpy(buf, &h, sizeof(h));
      std::memcpy(static_cast<char *>(buf) + sizeof(h), p, sizeof(S));
      return need;
    } else {
      return 0;
    }
  }
  static bool import_state(void *p, const void *buf, size_t len) noexcept {
    if constexpr (std::is_trivially_copyable_v<S>) {
      StateHeader h;
      if (len != sizeof(h) + sizeof(S))
        return false;
      std::memcpy(&h, buf, sizeof(h));
      if (h.magic != STATE_MAGIC || h.type_hash != name_hash(Name) ||
          h.size != sizeof(S))
        return false;
      std::memcpy(p, static_cast<const char *>(buf) + sizeof(h), sizeof(S));
      return true;
    } else {
      return false;
    }
  }
  static void destroy(void *p) noexcept { delete static_cast<S *>(p); }

  static constexpr ll_strategy_module_v1 table = {
      MODULE_ABI_VERSION, exec_abi_hash(), build_isa_mask(), 0, Name, LL_BUILD_ID,
      &create, &run_null, nullptr, &export_state, &import_state, &destroy};
};

// Strategies compiled into the executable (exec_main_<name>), resolved by
// "builtin:<name>" through the same table a module exports.
struct Builtins {
  static constexpr int MAX = 16;
  inline static const ll_strategy_module_v1 *table[MAX] = {};
  inline static int count = 0;
  static bool add(const ll_strategy_module_v1 *m) noexcept {
    if (count == MAX)
      return false;
    table[count++] = m;
    return true;
  }
  static const ll_strategy_module_v1 *find(const char *name) noexcept {
    for (int i = 0; i < count; ++i)
      if (std::strcmp(table[i]->name, name) == 0)
        return table[i];
    return nullptr;
  }
};

} // namespace exec::module_detail

// Exports the module entry point from a strategy_<name>.so.
#define LL_STRATEGY_MODULE(Type, NAME)                                          \
  namespace {                                                                   \
  constexpr char ll_module_name[] = NAME;                                       \
  }                                                                             \
  extern "C" __attribute__((visibility("default"))) const ll_strategy_module_v1 \
      *ll_strategy_module() {                                                   \
    return &exec::module_detail::Impl<Type, ll_module_name>::table;             \
  }

// Registers a strategy compiled into the executable as builtin:<NAME>.
#define LL_BUILTIN_STRATEGY(Type, NAME)                                         \
  namespace {                                                                   \
  constexpr char ll_builtin_name[] = NAME;                                      \
  const bool ll_builtin_registered = exec::module_detail::Builtins::add(        \
      &exec::module_detail::Impl<Type, ll_builtin_name>::table);                \
  }

#pragma once

#include "exec/module_abi.hpp"
#include <cstddef>

namespace exec {

struct LoadedModule {
  const ll_strategy_module_v1 *m = nullptr;
  void *handle = nullptr; // dlopen handle; nullptr for a builtin
  // Kept open while loaded: dlopen dedups by path, and a closed fd's number (so
  // its /proc/self/fd path) would be reused by the next module's memfd.
  int memfd = -1;
};

// Resolves a strategy spec: "builtin:<name>" or a bare name looks in the
// executable's builtins; anything containing '/' or ending in ".so" is loaded as a
// module. A module is checked (no PT_TLS, ABI version, layout hash, ISA), copied
// into a memfd and dlopen'd from /proc/self/fd (so rebuilding the file in place
// can't change the mapped code), then pre-faulted. On failure writes a reason to
// err and returns false.
bool load_strategy(const char *spec, LoadedModule &out, char *err,
                   size_t errlen) noexcept;

} // namespace exec

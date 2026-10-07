#pragma once

#include <cstdio>

// Minimal test harness shared by the standalone test executables.
inline int passed = 0;
inline int failed = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (cond) {                                                                \
      ++passed;                                                                \
    } else {                                                                   \
      ++failed;                                                                \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
    }                                                                          \
  } while (0)

// Prints the tally; use as main's return value.
inline int check_summary() {
  printf("%d passed, %d failed\n", passed, failed);
  return failed ? 1 : 0;
}

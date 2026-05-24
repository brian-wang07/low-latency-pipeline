#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>

#include "common/ipc/shm.hpp"

class ItchParser {
public:
  ItchParser(int fd, core::CoreRing *ring) noexcept;
  void next();
  bool done() const noexcept { return cursor_ >= end_; }

private:
  // convert 8 char sized ticker to uint64_t
  uint64_t tick_to_int(const char *data) {
    uint64_t val;
    memcpy(&val, data, 8);
    return val;
  }

  // convert uint64_t to ticker
  std::string int_to_tick(uint64_t val) {
    char buf[9] = {0};
    std::memcpy(buf, &val, 8);
    return std::string(buf);
  }

  // 48 bit big endian to unsigned 64 bit host
  uint64_t be48tou64h(const uint8_t *data) {
    uint64_t val = 0;
    std::memcpy(&val, data, 6);
    val = __builtin_bswap64(val);
    return val >> 16;
  }

  int fd_;
  core::CoreRing *ring_;
  struct stat fs_;
  const uint8_t *data_;
  const uint8_t *cursor_;
  const uint8_t *end_;
  uint64_t last_timestamp_ns_{0};
  bool first_event_{true};
};
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace esphome {
inline std::string format_hex_pretty(const uint8_t *data, size_t length) {
  std::string out;
  char part[4];
  for (size_t i = 0; i < length; i++) {
    snprintf(part, sizeof(part), i == 0 ? "%02X" : ".%02X", data[i]);
    out += part;
  }
  return out;
}
}  // namespace esphome

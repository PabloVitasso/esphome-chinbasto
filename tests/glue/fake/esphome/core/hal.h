#pragma once
// Fake ESPHome HAL for the glue tests. A manual clock: the test sets it.
#include <cstdint>

namespace esphome {
namespace fake {
inline uint32_t &clock_ms() {
  static uint32_t now = 0;
  return now;
}
}  // namespace fake

inline uint32_t millis() { return fake::clock_ms(); }
// delay() advances the fake clock, so a blocking delay shows in the test.
inline void delay(uint32_t ms) { fake::clock_ms() += ms; }
}  // namespace esphome

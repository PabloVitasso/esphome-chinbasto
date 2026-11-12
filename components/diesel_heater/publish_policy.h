#pragma once

// Publish policy. Pure C++, no ESPHome, no heap.
//
// Every MQTT message is a WiFi transmit burst, and the radio pays per
// wake-up, not per byte. So sends are aligned on one update, never
// spread. Three kinds of output, each with one rule:
//
// Discrete values (state, errors, demand, link): ChangeGate. A change goes
// out at once. Every value goes out again after the refresh period, so a
// consumer that missed one heals.
//
// Measurements (pump, fan, chamber): MinuteWindow. Every valid exchange
// adds a sample. Once per period the window gives the last pump value,
// the mean fan speed, and the last and the highest chamber temperature,
// then starts again. A peak between two sends is not lost.
//
// Debug values: no rule here. They go out as one JSON message per
// update, only while debug is on.

#include <cstdint>

namespace esphome {
namespace heater_uart {
namespace policy {

class ChangeGate {
 public:
  // True when `value` should go out now. Fails on the first rule that
  // holds it back.
  bool due(int32_t value, uint32_t now, uint32_t refresh_ms) const {
    if (!sent_)
      return true;
    if ((now - last_ms_) >= refresh_ms)
      return true;
    if (value != last_)
      return true;
    return false;
  }

  void mark(int32_t value, uint32_t now) {
    last_ = value;
    last_ms_ = now;
    sent_ = true;
  }

  // The next due() returns true, whatever the value.
  void reset() { sent_ = false; }

 private:
  int32_t last_{0};
  uint32_t last_ms_{0};
  bool sent_{false};
};

class MinuteWindow {
 public:
  // About one exchange per second, so one hour of samples. A cap keeps the
  // fan sum bounded if publishing ever stops.
  static constexpr uint32_t MAX_SAMPLES = 3600;

  void restart(uint32_t now) {
    start_ms_ = now;
    started_ = true;
    has_pump_ = false;
    has_chamber_ = false;
    fan_sum_ = 0;
    fan_count_ = 0;
  }

  void add_pump(int32_t raw) {
    pump_last_ = raw;
    has_pump_ = true;
  }

  void add_fan(int32_t rpm) {
    if (fan_count_ >= MAX_SAMPLES)
      return;
    if (rpm < 0)
      return;
    fan_sum_ += static_cast<uint32_t>(rpm);
    fan_count_++;
  }

  void add_chamber(int32_t celsius) {
    chamber_last_ = celsius;
    if (!has_chamber_)
      chamber_max_ = celsius;
    if (celsius > chamber_max_)
      chamber_max_ = celsius;
    has_chamber_ = true;
  }

  bool due(uint32_t now, uint32_t period_ms) const {
    if (!started_)
      return false;
    return (now - start_ms_) >= period_ms;
  }

  bool has_pump() const { return has_pump_; }
  bool has_fan() const { return fan_count_ > 0; }
  bool has_chamber() const { return has_chamber_; }
  int32_t pump_last() const { return pump_last_; }
  int32_t chamber_last() const { return chamber_last_; }
  int32_t chamber_max() const { return chamber_max_; }

  // 0 with no sample. Check has_fan() first.
  float fan_mean() const {
    if (fan_count_ == 0)
      return 0.0f;
    return static_cast<float>(fan_sum_) / static_cast<float>(fan_count_);
  }

 private:
  uint32_t start_ms_{0};
  bool started_{false};
  bool has_pump_{false};
  bool has_chamber_{false};
  int32_t pump_last_{0};
  int32_t chamber_last_{0};
  int32_t chamber_max_{0};
  uint32_t fan_sum_{0};
  uint32_t fan_count_{0};
};

}  // namespace policy
}  // namespace heater_uart
}  // namespace esphome

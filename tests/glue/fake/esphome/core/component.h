#pragma once
#include <cstdint>

namespace esphome {
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
};

class PollingComponent : public Component {
 public:
  explicit PollingComponent(uint32_t interval) : interval_(interval) {}
  virtual void update() = 0;
  uint32_t get_update_interval() const { return interval_; }

 protected:
  uint32_t interval_;
};
}  // namespace esphome

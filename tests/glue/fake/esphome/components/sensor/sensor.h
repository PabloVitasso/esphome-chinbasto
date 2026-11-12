#pragma once
#include <vector>

namespace esphome {
namespace sensor {
// Records every published value.
class Sensor {
 public:
  void publish_state(float value) {
    state = value;
    history.push_back(value);
  }
  float state{0};
  std::vector<float> history;
};
}  // namespace sensor
}  // namespace esphome

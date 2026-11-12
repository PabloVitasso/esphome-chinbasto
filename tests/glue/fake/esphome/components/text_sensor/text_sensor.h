#pragma once
#include <string>
#include <vector>

namespace esphome {
namespace text_sensor {
class TextSensor {
 public:
  void publish_state(const std::string &value) {
    state = value;
    history.push_back(value);
  }
  std::string state;
  std::vector<std::string> history;
};
}  // namespace text_sensor
}  // namespace esphome

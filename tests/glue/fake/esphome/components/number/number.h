#pragma once
#include <vector>

namespace esphome {
namespace number {
class Number {
 public:
  virtual ~Number() = default;
  void publish_state(float value) {
    state = value;
    history.push_back(value);
  }
  // The real Number calls control() from make_call().set_value().
  void set(float value) { control(value); }
  float state{0};
  std::vector<float> history;

 protected:
  virtual void control(float value) = 0;
};
}  // namespace number
}  // namespace esphome

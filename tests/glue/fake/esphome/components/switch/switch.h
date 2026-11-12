#pragma once
#include <cstdint>
#include <vector>

#include "esphome/core/hal.h"

namespace esphome {
namespace switch_ {
// Records each edge with the fake clock time.
struct Edge {
  bool on;
  uint32_t at_ms;
};

class Switch {
 public:
  void turn_on() { set_(true); }
  void turn_off() { set_(false); }
  bool state{false};
  std::vector<Edge> edges;

 private:
  void set_(bool on) {
    if (on != state)
      edges.push_back(Edge{on, millis()});
    state = on;
  }
};
}  // namespace switch_
}  // namespace esphome

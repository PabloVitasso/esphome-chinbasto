#pragma once
// Fake UART. The test queues RX bytes and reads back TX bytes.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace esphome {
namespace uart {
class UARTComponent {
 public:
  std::deque<uint8_t> rx;
  std::vector<uint8_t> tx;
  int flushes{0};
  // Bytes the bus echoes back after a write, as a shared RX/TX pin does.
  bool echo_tx{false};
  // Bytes the bus delivers right after the echo: the heater's reply.
  std::vector<uint8_t> reply_after_write;
  // When set, read_byte() fails once with data available.
  bool fail_next_read{false};
};

class UARTDevice {
 public:
  explicit UARTDevice(UARTComponent *parent) : parent_(parent) {}
  int available() { return static_cast<int>(parent_->rx.size()); }
  bool read_byte(uint8_t *data) {
    if (parent_->fail_next_read) {
      parent_->fail_next_read = false;
      return false;
    }
    if (parent_->rx.empty())
      return false;
    *data = parent_->rx.front();
    parent_->rx.pop_front();
    return true;
  }
  void write_array(const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) {
      parent_->tx.push_back(data[i]);
      if (parent_->echo_tx)
        parent_->rx.push_back(data[i]);
    }
    for (uint8_t b : parent_->reply_after_write)
      parent_->rx.push_back(b);
    parent_->reply_after_write.clear();
  }
  void flush() { parent_->flushes++; }

 protected:
  UARTComponent *parent_;
};
}  // namespace uart
}  // namespace esphome

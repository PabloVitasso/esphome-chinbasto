#pragma once

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "heater_protocol.h"
#include "publish_policy.h"

namespace esphome {
namespace heater_uart {

class HeaterUart;

class HeaterDutyNumber : public number::Number {
 public:
  explicit HeaterDutyNumber(HeaterUart *parent) : parent_(parent) {}

 protected:
  void control(float value) override;
  HeaterUart *parent_;
};

// Discrete values. Each has a sensor, which may be null, and a ChangeGate.
// Read publish_policy.h.
enum Discrete : uint8_t {
  D_STATE,
  D_ON_OFF,
  D_ERROR,
  D_ACTIVE_ERROR,
  D_DEMAND,
  D_LINK,
  D_COUNT
};

// Measurement sensors, sent once per window. Each may be null.
struct MeasurementSensors {
  sensor::Sensor *pump{nullptr};
  sensor::Sensor *fan{nullptr};
  sensor::Sensor *chamber{nullptr};
  sensor::Sensor *chamber_max{nullptr};
};

// Text sensors. Each may be null.
struct TextSensors {
  text_sensor::TextSensor *state_text{nullptr};
  text_sensor::TextSensor *error_text{nullptr};
  text_sensor::TextSensor *debug_data{nullptr};
};

// Rejected exchanges, by reason. Monotonic since boot.
struct Counters {
  uint32_t ok{0};
  uint32_t bad_start{0};
  uint32_t bad_marker{0};
  uint32_t bad_crc{0};
  uint32_t range_rejects{0};
};

class HeaterUart : public PollingComponent, public uart::UARTDevice {
 public:
  explicit HeaterUart(uart::UARTComponent *parent);

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  void request_on();
  void request_off();
  void request_duty(float value);
  void set_dump_packets(bool dump_packets) { dump_packets_ = dump_packets; }
  void set_duty_control(HeaterDutyNumber *duty_control,
                        switch_::Switch *up_switch,
                        switch_::Switch *down_switch);

  // Debug data goes out only while this is on. Off after every boot.
  void set_debug(bool on) { debug_ = on; }
  bool get_debug() const { return debug_; }

  void set_state_sensor(sensor::Sensor *s) { discrete_[D_STATE] = s; }
  void set_on_off_sensor(sensor::Sensor *s) { discrete_[D_ON_OFF] = s; }
  // Exchange byte 41: the last error, held until the next start.
  void set_error_sensor(sensor::Sensor *s) { discrete_[D_ERROR] = s; }
  // Exchange byte 27: the live error state.
  void set_active_error_sensor(sensor::Sensor *s) { discrete_[D_ACTIVE_ERROR] = s; }
  void set_demand_sensor(sensor::Sensor *s) { discrete_[D_DEMAND] = s; }
  void set_link_sensor(sensor::Sensor *s) { discrete_[D_LINK] = s; }

  void set_pump_sensor(sensor::Sensor *s) { measure_.pump = s; }
  void set_fan_sensor(sensor::Sensor *s) { measure_.fan = s; }
  void set_chamber_sensor(sensor::Sensor *s) { measure_.chamber = s; }
  void set_chamber_max_sensor(sensor::Sensor *s) { measure_.chamber_max = s; }

  void set_state_text_sensor(text_sensor::TextSensor *s) { text_.state_text = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s) { text_.error_text = s; }
  void set_debug_data_sensor(text_sensor::TextSensor *s) { text_.debug_data = s; }

 protected:
  enum class PendingCommand : uint8_t { NONE, ON, OFF };
  enum class DutyPulseState : uint8_t { IDLE, HIGH, LOW };

  // Receive path.
  void read_bus_();
  bool on_exchange_(const uint8_t *exchange);
  void count_check_(protocol::Check check);
  void add_to_window_(uint32_t now);
  bool link_up_(uint32_t now) const;

  // Publish path. Read publish_policy.h.
  bool publish_discrete_value_(Discrete d, int32_t value, uint32_t now);
  void publish_discrete_(uint32_t now);
  void publish_window_(uint32_t now);
  void publish_duty_control_();
  void publish_debug_(uint32_t now);
  bool format_debug_(char *out, size_t size, uint32_t now) const;

  // Serial command path.
  void expire_pending_command_();
  bool command_slot_fresh_(uint32_t loop_gap_ms);
  void arm_command_(uint32_t now);
  void service_command_tx_();
  void send_pending_command_();
  bool skip_echo_byte_(uint32_t now);

  // Duty control path.
  void start_duty_burst_();
  void service_duty_pulse_();
  void handle_duty_telemetry_();
  void cancel_duty_burst_();
  void press_duty_switch_(bool on);

  sensor::Sensor *discrete_[D_COUNT]{};
  policy::ChangeGate gate_[D_COUNT]{};
  MeasurementSensors measure_;
  policy::MinuteWindow window_;
  TextSensors text_;
  bool debug_{false};
  bool link_was_up_{false};
  bool link_lost_{false};

  protocol::Receiver receiver_;
  protocol::Telemetry telemetry_{};
  Counters counters_;
  bool have_telemetry_{false};
  uint32_t last_good_ms_{0};
  uint32_t last_rx_byte_ms_{0};
  uint32_t last_loop_ms_{0};
  uint8_t last_exchange_[protocol::EXCHANGE_LEN]{};

  // Last documented values. -1 means none yet.
  int32_t run_state_{-1};
  int32_t active_error_{-1};
  int32_t last_error_{-1};
  int32_t demand_{-1};

  PendingCommand pending_command_{PendingCommand::NONE};
  uint32_t pending_command_requested_ms_{0};
  // An armed command goes out SERIAL_COMMAND_DELAY_MS after its exchange,
  // from loop(), never by blocking.
  bool tx_armed_{false};
  uint32_t tx_armed_ms_{0};
  // Bytes of our own transmission still to drop from the receive path.
  uint8_t echo_left_{0};
  uint32_t echo_until_ms_{0};
  uint8_t controller_frame_[protocol::FRAME_LEN]{};
  bool dump_packets_{false};

  HeaterDutyNumber *duty_control_{nullptr};
  switch_::Switch *up_switch_{nullptr};
  switch_::Switch *down_switch_{nullptr};
  int duty_target_{-1};
  bool duty_target_active_{false};
  bool duty_waiting_for_feedback_{false};
  bool duty_direction_up_{false};
  uint8_t duty_pulses_remaining_{0};
  uint8_t duty_correction_rounds_{0};
  uint8_t duty_stable_count_{0};
  int duty_stable_candidate_{-1};
  int duty_stable_value_{-1};
  DutyPulseState duty_pulse_state_{DutyPulseState::IDLE};
  uint32_t duty_pulse_phase_ms_{0};
  uint32_t duty_feedback_wait_started_ms_{0};
  uint32_t duty_target_requested_ms_{0};
};

}  // namespace heater_uart
}  // namespace esphome

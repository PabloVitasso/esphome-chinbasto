#pragma once

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"

#include "heater_protocol.h"

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

// Optional sensors. Each one may be null. publish_() checks.
struct OptionalSensors {
  sensor::Sensor *active_error{nullptr};
  // Diagnostics.
  sensor::Sensor *link{nullptr};
  sensor::Sensor *frame_age{nullptr};
  sensor::Sensor *frames_ok{nullptr};
  sensor::Sensor *frames_bad_start{nullptr};
  sensor::Sensor *frames_bad_marker{nullptr};
  sensor::Sensor *frames_bad_crc{nullptr};
  sensor::Sensor *range_rejects{nullptr};
  // Research: raw protocol fields, unverified meaning.
  sensor::Sensor *ctrl_temperature_raw{nullptr};
  sensor::Sensor *ctrl_pump_min_raw{nullptr};
  sensor::Sensor *ctrl_pump_max_raw{nullptr};
  sensor::Sensor *ctrl_fan_min_raw{nullptr};
  sensor::Sensor *ctrl_fan_max_raw{nullptr};
  sensor::Sensor *ctrl_voltage_raw{nullptr};
  sensor::Sensor *ctrl_mode_raw{nullptr};
  sensor::Sensor *supply_voltage_raw{nullptr};
  sensor::Sensor *fan_voltage_raw{nullptr};
  sensor::Sensor *glow_voltage_raw{nullptr};
  sensor::Sensor *glow_current_raw{nullptr};
  text_sensor::TextSensor *exchange_hex{nullptr};
  text_sensor::TextSensor *state_text{nullptr};
  text_sensor::TextSensor *error_text{nullptr};
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
  HeaterUart(uart::UARTComponent *parent,
             sensor::Sensor *set_temp,
             sensor::Sensor *heater_state_int,
             sensor::Sensor *heater_error_int,
             sensor::Sensor *on_off,
             sensor::Sensor *pump_freq,
             sensor::Sensor *fan_speed,
             sensor::Sensor *chamber_temp,
             sensor::Sensor *duty_cycle);

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

  // Upstream setters, kept by name.
  void set_state_text_sensor(text_sensor::TextSensor *s) { opt_.state_text = s; }
  void set_error_text_sensor(text_sensor::TextSensor *s) { opt_.error_text = s; }
  // Response byte 3: the live error state. `error` holds the last error.
  void set_active_error_sensor(sensor::Sensor *s) { opt_.active_error = s; }

  void set_link_sensor(sensor::Sensor *s) { opt_.link = s; }
  void set_frame_age_sensor(sensor::Sensor *s) { opt_.frame_age = s; }
  void set_frames_ok_sensor(sensor::Sensor *s) { opt_.frames_ok = s; }
  void set_frames_bad_start_sensor(sensor::Sensor *s) { opt_.frames_bad_start = s; }
  void set_frames_bad_marker_sensor(sensor::Sensor *s) { opt_.frames_bad_marker = s; }
  void set_frames_bad_crc_sensor(sensor::Sensor *s) { opt_.frames_bad_crc = s; }
  void set_range_rejects_sensor(sensor::Sensor *s) { opt_.range_rejects = s; }

  void set_ctrl_temperature_raw_sensor(sensor::Sensor *s) { opt_.ctrl_temperature_raw = s; }
  void set_ctrl_pump_min_raw_sensor(sensor::Sensor *s) { opt_.ctrl_pump_min_raw = s; }
  void set_ctrl_pump_max_raw_sensor(sensor::Sensor *s) { opt_.ctrl_pump_max_raw = s; }
  void set_ctrl_fan_min_raw_sensor(sensor::Sensor *s) { opt_.ctrl_fan_min_raw = s; }
  void set_ctrl_fan_max_raw_sensor(sensor::Sensor *s) { opt_.ctrl_fan_max_raw = s; }
  void set_ctrl_voltage_raw_sensor(sensor::Sensor *s) { opt_.ctrl_voltage_raw = s; }
  void set_ctrl_mode_raw_sensor(sensor::Sensor *s) { opt_.ctrl_mode_raw = s; }
  void set_supply_voltage_raw_sensor(sensor::Sensor *s) { opt_.supply_voltage_raw = s; }
  void set_fan_voltage_raw_sensor(sensor::Sensor *s) { opt_.fan_voltage_raw = s; }
  void set_glow_voltage_raw_sensor(sensor::Sensor *s) { opt_.glow_voltage_raw = s; }
  void set_glow_current_raw_sensor(sensor::Sensor *s) { opt_.glow_current_raw = s; }
  void set_exchange_hex_sensor(text_sensor::TextSensor *s) { opt_.exchange_hex = s; }

 protected:
  enum class PendingCommand : uint8_t { NONE, ON, OFF };
  enum class DutyPulseState : uint8_t { IDLE, HIGH, LOW };

  // Receive path.
  void read_bus_();
  void on_exchange_(const uint8_t *exchange);
  void count_check_(protocol::Check check);
  bool link_up_(uint32_t now) const;

  // Publish path.
  void publish_discrete_();
  void publish_measurements_();
  void publish_research_();
  void publish_diagnostics_(uint32_t now);
  void publish_exchange_hex_(uint32_t now);

  // Serial command path.
  void expire_pending_command_();
  void send_pending_command_();

  // Duty control path.
  void start_duty_burst_();
  void service_duty_pulse_();
  void handle_duty_telemetry_();
  void cancel_duty_burst_();
  void press_duty_switch_(bool on);

  sensor::Sensor *set_temp_;
  sensor::Sensor *heater_state_int_;
  sensor::Sensor *heater_error_int_;
  sensor::Sensor *on_off_;
  sensor::Sensor *pump_freq_;
  sensor::Sensor *fan_speed_;
  sensor::Sensor *chamber_temp_;
  sensor::Sensor *duty_cycle_;
  OptionalSensors opt_;

  protocol::Receiver receiver_;
  protocol::Telemetry telemetry_{};
  Counters counters_;
  bool have_telemetry_{false};
  uint32_t last_good_ms_{0};
  uint32_t last_rx_byte_ms_{0};

  // Last documented values. -1 means none yet.
  int32_t run_state_{-1};
  int32_t active_error_{-1};
  int32_t last_error_{-1};
  int32_t demand_{-1};
  int32_t published_state_{-1};
  int32_t published_error_{-1};
  int32_t published_active_error_{-1};
  int32_t published_on_off_{-1};

  uint8_t last_hex_exchange_[protocol::EXCHANGE_LEN]{};
  bool hex_published_once_{false};
  uint32_t last_hex_ms_{0};

  // Serial command timing: counts the 48 bytes after a bus-idle gap.
  int command_sync_count_{0};
  bool command_sync_active_{false};
  PendingCommand pending_command_{PendingCommand::NONE};
  uint32_t pending_command_requested_ms_{0};
  uint8_t controller_frame_[protocol::FRAME_LEN]{};
  bool controller_frame_valid_{false};
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

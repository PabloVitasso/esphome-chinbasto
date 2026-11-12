#include "heater_uart.h"

#include <cmath>
#include <cstdio>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace heater_uart {

namespace p = protocol;

static const char *const TAG = "heater_uart";

// A bus-idle gap this long starts a new exchange.
static const uint32_t IDLE_GAP_MS = 100;
// With no valid exchange for this long, the link is down. Measurements
// then stop publishing, so a consumer sees them age instead of seeing a
// stale value that looks fresh.
static const uint32_t LINK_TIMEOUT_MS = 10000;
// Bytes read per loop() call. The bus runs 25000 baud, about 2.5 bytes
// per ms, so 128 bytes covers any loop() interval in practice and bounds
// the time spent here.
static const size_t MAX_BYTES_PER_LOOP = 128;
// The raw exchange is published on change, at most this often.
static const uint32_t HEX_MIN_INTERVAL_MS = 30000;

static const uint32_t SERIAL_COMMAND_TIMEOUT_MS = 3000;
static const uint32_t SERIAL_COMMAND_DELAY_MS = 50;
static const uint8_t CMD_ON = 0xA0;
static const uint8_t CMD_OFF = 0x05;
static const size_t CMD_INDEX = 2;

static const uint32_t DUTY_TARGET_TIMEOUT_MS = 60000;
static const uint32_t DUTY_PULSE_MS = 100;
static const uint32_t DUTY_FEEDBACK_DELAY_MS = 5000;
static const uint8_t DUTY_MAX_CORRECTION_ROUNDS = 5;
static const uint8_t DUTY_STABLE_SAMPLES = 2;

static const char *heater_state_text(int32_t state) {
  switch (state) {
    case 0: return "Off";
    case 1: return "Starting";
    case 2: return "Pre-Heat";
    case 3: return "Failed Start - Retrying";
    case 4: return "Ignition - Heating Up";
    case 5: return "Running Normally";
    case 6: return "Stop Command Received";
    case 7: return "Stopping";
    case 8: return "Cooldown";
    default: return "Unknown";
  }
}

static const char *heater_error_text(int32_t error) {
  switch (error) {
    case 0: return "No Error";
    case 1: return "No Error - Started";
    case 2: return "Voltage Too Low";
    case 3: return "Voltage Too High";
    case 4: return "Ignition Plug Failure";
    case 5: return "Pump Overcurrent";
    case 6: return "Over Temperature";
    case 7: return "Motor Failure";
    case 8: return "Serial Connection Lost";
    case 9: return "Flame Out";
    case 10: return "Temperature Sensor Failure";
    case 11: return "Ignition Failed";
    case 12: return "First Ignition Attempt Failed";
    case 13: return "Excess Fuel Usage";
    default: return "Unknown";
  }
}

static void publish_if(sensor::Sensor *target, float value) {
  if (target == nullptr)
    return;
  target->publish_state(value);
}

static void publish_field(sensor::Sensor *target, const p::Field &field, float scale) {
  if (target == nullptr)
    return;
  if (!field.valid)
    return;
  target->publish_state(static_cast<float>(field.value) * scale);
}

HeaterUart::HeaterUart(uart::UARTComponent *parent,
                       sensor::Sensor *set_temp,
                       sensor::Sensor *heater_state_int,
                       sensor::Sensor *heater_error_int,
                       sensor::Sensor *on_off,
                       sensor::Sensor *pump_freq,
                       sensor::Sensor *fan_speed,
                       sensor::Sensor *chamber_temp,
                       sensor::Sensor *duty_cycle)
    : PollingComponent(5000), UARTDevice(parent),
      set_temp_(set_temp),
      heater_state_int_(heater_state_int),
      heater_error_int_(heater_error_int),
      on_off_(on_off),
      pump_freq_(pump_freq),
      fan_speed_(fan_speed),
      chamber_temp_(chamber_temp),
      duty_cycle_(duty_cycle) {}

void HeaterUart::setup() {
  last_rx_byte_ms_ = millis();
}

void HeaterUart::dump_config() {
  ESP_LOGCONFIG(TAG, "Diesel heater (blue wire):");
  ESP_LOGCONFIG(TAG, "  Link timeout: %u ms", static_cast<unsigned>(LINK_TIMEOUT_MS));
  ESP_LOGCONFIG(TAG, "  Dump packets: %s", YESNO(dump_packets_));
  ESP_LOGCONFIG(TAG, "  Duty control: %s", YESNO(duty_control_ != nullptr));
  LOG_UPDATE_INTERVAL(this);
}

// ---------------------------------------------------------------- loop

void HeaterUart::loop() {
  service_duty_pulse_();
  expire_pending_command_();
  read_bus_();
}

void HeaterUart::read_bus_() {
  bool command_slot = false;
  for (size_t n = 0; n < MAX_BYTES_PER_LOOP; n++) {
    uint8_t value = 0;
    if (!available())
      break;
    if (!read_byte(&value))
      break;

    const uint32_t now = millis();
    const bool idle_gap = (now - last_rx_byte_ms_) > IDLE_GAP_MS;
    last_rx_byte_ms_ = now;

    // Command timing follows the 48 bytes after an idle gap. It never
    // publishes values from that looser framing.
    if (idle_gap) {
      command_sync_active_ = true;
      command_sync_count_ = 0;
    }
    if (command_sync_active_) {
      command_sync_count_++;
      if (command_sync_count_ == static_cast<int>(p::EXCHANGE_LEN)) {
        command_sync_active_ = false;
        command_slot = true;
      }
    }

    // Process a complete exchange now: the next byte reuses the buffer.
    if (receiver_.feed(value, idle_gap))
      on_exchange_(receiver_.exchange());
    // A command goes out right after the exchange, as upstream does.
    if (command_slot)
      break;
  }

  if (command_slot)
    send_pending_command_();
}

void HeaterUart::count_check_(p::Check check) {
  switch (check) {
    case p::Check::OK:
      counters_.ok++;
      return;
    case p::Check::BAD_START:
      counters_.bad_start++;
      return;
    case p::Check::BAD_MARKER:
      counters_.bad_marker++;
      return;
    case p::Check::BAD_CRC_CONTROLLER:
    case p::Check::BAD_CRC_HEATER:
      counters_.bad_crc++;
      return;
  }
}

void HeaterUart::on_exchange_(const uint8_t *exchange) {
  const p::Check check = p::check_exchange(exchange);
  count_check_(check);
  if (check != p::Check::OK)
    return;

  if (dump_packets_) {
    ESP_LOGD(TAG, "RX exchange [48 bytes]: %s",
             format_hex_pretty(exchange, p::EXCHANGE_LEN).c_str());
  }

  for (size_t i = 0; i < p::FRAME_LEN; i++)
    controller_frame_[i] = exchange[i];
  controller_frame_valid_ = true;

  telemetry_ = p::decode_exchange(exchange);
  counters_.range_rejects += telemetry_.range_rejects;
  have_telemetry_ = true;
  last_good_ms_ = millis();

  // Hold the last documented value when a field is not valid.
  if (telemetry_.run_state.valid)
    run_state_ = telemetry_.run_state.value;
  if (telemetry_.active_error.valid)
    active_error_ = telemetry_.active_error.value;
  if (telemetry_.last_error.valid)
    last_error_ = telemetry_.last_error.value;
  if (telemetry_.demand.valid)
    demand_ = telemetry_.demand.value;

  for (size_t i = 0; i < p::EXCHANGE_LEN; i++) {
    if (last_hex_exchange_[i] != exchange[i])
      hex_published_once_ = false;
    last_hex_exchange_[i] = exchange[i];
  }

  publish_discrete_();
  handle_duty_telemetry_();
}

bool HeaterUart::link_up_(uint32_t now) const {
  if (!have_telemetry_)
    return false;
  if ((now - last_good_ms_) > LINK_TIMEOUT_MS)
    return false;
  return true;
}

// ---------------------------------------------------------------- publish

// State, errors and power: on change, from loop(). update() repeats them,
// so a lost message heals within one interval.
void HeaterUart::publish_discrete_() {
  if (run_state_ >= 0 && run_state_ != published_state_) {
    heater_state_int_->publish_state(run_state_);
    if (opt_.state_text != nullptr)
      opt_.state_text->publish_state(heater_state_text(run_state_));
    published_state_ = run_state_;
  }
  if (last_error_ >= 0 && last_error_ != published_error_) {
    heater_error_int_->publish_state(last_error_);
    if (opt_.error_text != nullptr)
      opt_.error_text->publish_state(heater_error_text(last_error_));
    published_error_ = last_error_;
  }
  if (active_error_ >= 0 && active_error_ != published_active_error_) {
    publish_if(opt_.active_error, active_error_);
    published_active_error_ = active_error_;
  }
  if (run_state_ >= 0) {
    const int32_t power = p::power_from_run_state(run_state_);
    if (power != published_on_off_) {
      on_off_->publish_state(power);
      published_on_off_ = power;
    }
  }
}

void HeaterUart::publish_measurements_() {
  if (run_state_ >= 0) {
    heater_state_int_->publish_state(run_state_);
    on_off_->publish_state(p::power_from_run_state(run_state_));
  }
  if (last_error_ >= 0)
    heater_error_int_->publish_state(last_error_);
  if (active_error_ >= 0)
    publish_if(opt_.active_error, active_error_);
  if (demand_ >= 0) {
    set_temp_->publish_state(demand_);
    duty_cycle_->publish_state(p::duty_from_demand(demand_));
  }
  publish_field(pump_freq_, telemetry_.pump_raw, 0.1f);
  publish_field(fan_speed_, telemetry_.fan_rpm, 1.0f);
  publish_field(chamber_temp_, telemetry_.chamber_c, 1.0f);
  if (duty_control_ == nullptr)
    return;
  if (duty_target_active_)
    return;
  if (duty_stable_value_ < p::limits::DEMAND.min)
    return;
  duty_control_->publish_state(duty_stable_value_);
}

void HeaterUart::publish_research_() {
  const p::Telemetry &t = telemetry_;
  publish_if(opt_.ctrl_temperature_raw, t.ctrl_temperature_raw);
  publish_if(opt_.ctrl_pump_min_raw, t.ctrl_pump_min_raw);
  publish_if(opt_.ctrl_pump_max_raw, t.ctrl_pump_max_raw);
  publish_if(opt_.ctrl_fan_min_raw, t.ctrl_fan_min_raw);
  publish_if(opt_.ctrl_fan_max_raw, t.ctrl_fan_max_raw);
  publish_if(opt_.ctrl_voltage_raw, t.ctrl_voltage_raw);
  publish_if(opt_.ctrl_mode_raw, t.ctrl_mode_raw);
  publish_if(opt_.supply_voltage_raw, t.supply_voltage_raw);
  publish_if(opt_.fan_voltage_raw, t.fan_voltage_raw);
  publish_if(opt_.glow_voltage_raw, t.glow_voltage_raw);
  publish_if(opt_.glow_current_raw, t.glow_current_raw);
}

void HeaterUart::publish_diagnostics_(uint32_t now) {
  const bool up = link_up_(now);
  publish_if(opt_.link, up ? 1.0f : 0.0f);
  if (have_telemetry_)
    publish_if(opt_.frame_age, static_cast<float>(now - last_good_ms_) / 1000.0f);
  publish_if(opt_.frames_ok, counters_.ok);
  publish_if(opt_.frames_bad_start, counters_.bad_start);
  publish_if(opt_.frames_bad_marker, counters_.bad_marker);
  publish_if(opt_.frames_bad_crc, counters_.bad_crc);
  publish_if(opt_.range_rejects, counters_.range_rejects);
}

void HeaterUart::publish_exchange_hex_(uint32_t now) {
  if (opt_.exchange_hex == nullptr)
    return;
  if (hex_published_once_)
    return;
  if (last_hex_ms_ != 0 && (now - last_hex_ms_) < HEX_MIN_INTERVAL_MS)
    return;
  char text[p::EXCHANGE_LEN * 2 + 1];
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    snprintf(&text[i * 2], 3, "%02X", last_hex_exchange_[i]);
  opt_.exchange_hex->publish_state(text);
  hex_published_once_ = true;
  last_hex_ms_ = now;
}

void HeaterUart::update() {
  const uint32_t now = millis();
  publish_diagnostics_(now);
  if (!link_up_(now))
    return;
  publish_measurements_();
  publish_research_();
  publish_exchange_hex_(now);
}

// ---------------------------------------------------------------- serial commands

void HeaterUart::request_on() {
  ESP_LOGD(TAG, "Serial ON requested");
  pending_command_ = PendingCommand::ON;
  pending_command_requested_ms_ = millis();
}

void HeaterUart::request_off() {
  ESP_LOGD(TAG, "Serial OFF requested");
  pending_command_ = PendingCommand::OFF;
  pending_command_requested_ms_ = millis();
}

void HeaterUart::expire_pending_command_() {
  if (pending_command_ == PendingCommand::NONE)
    return;
  if (millis() - pending_command_requested_ms_ <= SERIAL_COMMAND_TIMEOUT_MS)
    return;
  ESP_LOGW(TAG, "Queued serial command expired before a valid exchange");
  pending_command_ = PendingCommand::NONE;
}

void HeaterUart::send_pending_command_() {
  if (pending_command_ == PendingCommand::NONE)
    return;
  if (!controller_frame_valid_)
    return;

  // Copy the controller's latest packet so its mode, demand, pump and fan
  // limits and voltage stay. Change only the command byte and the CRC.
  uint8_t command[p::FRAME_LEN];
  for (size_t i = 0; i < p::FRAME_LEN; i++)
    command[i] = controller_frame_[i];
  command[CMD_INDEX] = (pending_command_ == PendingCommand::ON) ? CMD_ON : CMD_OFF;
  p::frame_crc_write(command);

  if (dump_packets_) {
    ESP_LOGD(TAG, "TX [24 bytes]: %s", format_hex_pretty(command, p::FRAME_LEN).c_str());
  }

  // The timing of the known working implementation.
  delay(SERIAL_COMMAND_DELAY_MS);
  write_array(command, p::FRAME_LEN);
  flush();

  // A shared RX/TX pin echoes the local TX. Discard at most our 24 bytes.
  size_t echo = 0;
  while (echo < p::FRAME_LEN && available()) {
    uint8_t discard = 0;
    if (!read_byte(&discard))
      break;
    echo++;
  }

  // Never carry a partial exchange across our own transmission.
  receiver_.reset();
  command_sync_active_ = false;
  command_sync_count_ = 0;
  last_rx_byte_ms_ = millis();
  pending_command_ = PendingCommand::NONE;
  ESP_LOGD(TAG, "Serial command sent, %u echo bytes discarded", static_cast<unsigned>(echo));
}

// ---------------------------------------------------------------- duty control

void HeaterDutyNumber::control(float value) { parent_->request_duty(value); }

void HeaterUart::set_duty_control(HeaterDutyNumber *duty_control,
                                  switch_::Switch *up_switch,
                                  switch_::Switch *down_switch) {
  duty_control_ = duty_control;
  up_switch_ = up_switch;
  down_switch_ = down_switch;
}

void HeaterUart::request_duty(float value) {
  if (duty_control_ == nullptr)
    return;
  int target = static_cast<int>(value + 0.5f);
  if (target < p::limits::DEMAND.min)
    target = p::limits::DEMAND.min;
  if (target > p::limits::DEMAND.max)
    target = p::limits::DEMAND.max;

  ESP_LOGD(TAG, "Duty target requested: %d", target);
  duty_target_ = target;
  duty_target_active_ = true;
  duty_target_requested_ms_ = millis();
  duty_correction_rounds_ = 0;
  duty_control_->publish_state(target);

  // A changed target stops the current burst. Wait for fresh telemetry
  // before the replacement burst, so presses already accepted count.
  if (duty_pulse_state_ != DutyPulseState::IDLE || duty_waiting_for_feedback_) {
    cancel_duty_burst_();
    duty_stable_candidate_ = -1;
    duty_stable_count_ = 0;
    duty_waiting_for_feedback_ = true;
    duty_feedback_wait_started_ms_ = millis();
    return;
  }
  duty_waiting_for_feedback_ = false;
  start_duty_burst_();
}

void HeaterUart::press_duty_switch_(bool on) {
  switch_::Switch *target = duty_direction_up_ ? up_switch_ : down_switch_;
  if (target == nullptr)
    return;
  if (on) {
    target->turn_on();
  } else {
    target->turn_off();
  }
}

void HeaterUart::cancel_duty_burst_() {
  if (up_switch_ != nullptr)
    up_switch_->turn_off();
  if (down_switch_ != nullptr)
    down_switch_->turn_off();
  duty_pulse_state_ = DutyPulseState::IDLE;
  duty_pulses_remaining_ = 0;
}

void HeaterUart::start_duty_burst_() {
  if (!duty_target_active_)
    return;
  if (duty_control_ == nullptr)
    return;
  if (up_switch_ == nullptr)
    return;
  if (down_switch_ == nullptr)
    return;
  if (!p::in_range(duty_stable_value_, p::limits::DEMAND))
    return;

  const int current = duty_stable_value_;
  const int difference = duty_target_ - current;
  if (difference == 0) {
    duty_target_active_ = false;
    duty_waiting_for_feedback_ = false;
    duty_control_->publish_state(current);
    ESP_LOGD(TAG, "Duty target %d confirmed", current);
    return;
  }

  const bool out_of_rounds = duty_correction_rounds_ >= DUTY_MAX_CORRECTION_ROUNDS;
  const bool out_of_time = (millis() - duty_target_requested_ms_) > DUTY_TARGET_TIMEOUT_MS;
  if (out_of_rounds || out_of_time) {
    ESP_LOGW(TAG, "Duty target %d not reached; controller remains at %d", duty_target_, current);
    duty_target_active_ = false;
    duty_waiting_for_feedback_ = false;
    duty_control_->publish_state(current);
    return;
  }

  duty_direction_up_ = difference > 0;
  duty_pulses_remaining_ = static_cast<uint8_t>(difference > 0 ? difference : -difference);
  duty_correction_rounds_++;
  duty_waiting_for_feedback_ = false;
  duty_pulse_state_ = DutyPulseState::HIGH;
  duty_pulse_phase_ms_ = millis();
  duty_stable_candidate_ = -1;
  duty_stable_count_ = 0;
  ESP_LOGD(TAG, "Duty correction burst: %u %s pulse(s)", duty_pulses_remaining_,
           duty_direction_up_ ? "UP" : "DOWN");
  press_duty_switch_(true);
}

void HeaterUart::service_duty_pulse_() {
  if (duty_pulse_state_ == DutyPulseState::IDLE)
    return;
  const uint32_t now = millis();
  if (now - duty_pulse_phase_ms_ < DUTY_PULSE_MS)
    return;
  duty_pulse_phase_ms_ = now;

  if (duty_pulse_state_ == DutyPulseState::HIGH) {
    press_duty_switch_(false);
    duty_pulse_state_ = DutyPulseState::LOW;
    return;
  }

  duty_pulses_remaining_--;
  if (duty_pulses_remaining_ == 0) {
    duty_pulse_state_ = DutyPulseState::IDLE;
    duty_waiting_for_feedback_ = true;
    duty_feedback_wait_started_ms_ = now;
    duty_stable_candidate_ = -1;
    duty_stable_count_ = 0;
    return;
  }
  duty_pulse_state_ = DutyPulseState::HIGH;
  press_duty_switch_(true);
}

void HeaterUart::handle_duty_telemetry_() {
  if (duty_control_ == nullptr)
    return;
  if (!telemetry_.demand.valid)
    return;

  const int current = telemetry_.demand.value;
  if (current == duty_stable_candidate_) {
    if (duty_stable_count_ < DUTY_STABLE_SAMPLES)
      duty_stable_count_++;
  } else {
    duty_stable_candidate_ = current;
    duty_stable_count_ = 1;
  }
  if (duty_stable_count_ < DUTY_STABLE_SAMPLES)
    return;
  duty_stable_value_ = current;

  if (!duty_target_active_)
    return;
  // Let every burst finish. Then give the controller five seconds before
  // trusting its value.
  if (duty_pulse_state_ != DutyPulseState::IDLE)
    return;
  if (duty_waiting_for_feedback_ &&
      (millis() - duty_feedback_wait_started_ms_) < DUTY_FEEDBACK_DELAY_MS)
    return;

  if (duty_stable_value_ == duty_target_) {
    cancel_duty_burst_();
    duty_target_active_ = false;
    duty_waiting_for_feedback_ = false;
    duty_control_->publish_state(duty_stable_value_);
    ESP_LOGD(TAG, "Duty target %d confirmed", duty_stable_value_);
    return;
  }
  // The first burst is immediate. A corrective burst waits for feedback.
  if (!duty_waiting_for_feedback_ && duty_correction_rounds_ == 0) {
    start_duty_burst_();
    return;
  }
  if (duty_waiting_for_feedback_) {
    duty_waiting_for_feedback_ = false;
    start_duty_burst_();
  }
}

}  // namespace heater_uart
}  // namespace esphome

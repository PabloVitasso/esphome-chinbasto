#include "heater_uart.h"

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
// Discrete values: a change goes out at once, so a 5 min refresh is
// enough to heal a consumer that missed one.
static const uint32_t DISCRETE_REFRESH_MS = 300000;
// Measurements: one window per minute. A consumer sees them at most 60 s
// old, and the window keeps the chamber peak.
static const uint32_t MEASUREMENT_WINDOW_MS = 60000;
// One debug JSON message, about 600 bytes with the 96-character hex.
static const size_t DEBUG_JSON_SIZE = 768;

// A serial command goes out only if read_bus_() started within this long
// after the previous call. The UART gives no arrival time. A longer gap
// means the loop stalled, so the exchange may have ended long ago, and a
// late frame could collide with the panel's next frame. ESPHome runs the
// loop about every 16 ms.
static const uint32_t COMMAND_MAX_LOOP_GAP_MS = 30;
static const uint32_t SERIAL_COMMAND_TIMEOUT_MS = 3000;
// The heater takes a command this long after the end of an exchange: the
// timing of the known working implementation. Waited for in loop(), never
// with delay().
static const uint32_t SERIAL_COMMAND_DELAY_MS = 50;
// Our own 24 bytes echo back on the shared pin. Sending them takes about
// 10 ms at 25000 baud. Bytes after this window are the bus again.
static const uint32_t ECHO_WINDOW_MS = 40;
static const uint8_t CMD_ON = 0xA0;
static const uint8_t CMD_OFF = 0x05;
static const size_t CMD_INDEX = 2;

static const uint32_t DUTY_TARGET_TIMEOUT_MS = 60000;
static const uint32_t DUTY_PULSE_MS = 100;
static const uint32_t DUTY_FEEDBACK_DELAY_MS = 5000;
static const uint8_t DUTY_MAX_CORRECTION_ROUNDS = 5;
static const uint8_t DUTY_STABLE_SAMPLES = 2;

// The texts Node-RED and log.db have always used. Keep them stable.
static const char *heater_state_text(int32_t state) {
  switch (state) {
    case 0: return "Off";
    case 1: return "Starting";
    case 2: return "Pre-Heat";
    case 3: return "Failed Start - Retrying";
    case 4: return "Ignition - Now heating up";
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
    case 1: return "1: No Error, but started";
    case 2: return "E-01 Voltage too low";
    case 3: return "E-02 Voltage too high";
    case 4: return "E-03 Ignition plug failure";
    case 5: return "E-04 Pump Failure – over current";
    case 6: return "E-05 Too hot";
    case 7: return "E-06 Motor Failure";
    case 8: return "E-07 Serial connection lost";
    case 9: return "9: Gone-Out";
    case 10: return "E-09 Temperature sensor failure";
    case 11: return "E-10 Ignition fail/1st run fail";
    case 12: return "12: Failed 1st ignite";
    case 13: return "13: Excess fuel usage";
    default: return "Unknown";
  }
}

// Writes `len` bytes as upper-case hex plus a terminating zero. `out`
// holds at least 2 * len + 1 chars.
static void to_hex(const uint8_t *data, size_t len, char *out) {
  static const char DIGITS[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[i * 2] = DIGITS[data[i] >> 4];
    out[i * 2 + 1] = DIGITS[data[i] & 0x0F];
  }
  out[len * 2] = '\0';
}

static void publish_text(text_sensor::TextSensor *target, const char *text) {
  if (target == nullptr)
    return;
  target->publish_state(text);
}

HeaterUart::HeaterUart(uart::UARTComponent *parent)
    : PollingComponent(5000), UARTDevice(parent) {}

void HeaterUart::setup() {
  last_rx_byte_ms_ = millis();
}

void HeaterUart::dump_config() {
  ESP_LOGCONFIG(TAG, "Diesel heater (blue wire):");
  ESP_LOGCONFIG(TAG, "  Link timeout: %u ms", static_cast<unsigned>(LINK_TIMEOUT_MS));
  ESP_LOGCONFIG(TAG, "  Dump packets: %s", YESNO(dump_packets_));
  ESP_LOGCONFIG(TAG, "  Duty control: %s", YESNO(duty_control_ != nullptr));
  ESP_LOGCONFIG(TAG, "  Discrete: on change, refresh %u s", static_cast<unsigned>(DISCRETE_REFRESH_MS / 1000));
  ESP_LOGCONFIG(TAG, "  Measurement window: %u s", static_cast<unsigned>(MEASUREMENT_WINDOW_MS / 1000));
  ESP_LOGCONFIG(TAG, "  Debug data: %s", YESNO(text_.debug_data != nullptr));
  LOG_UPDATE_INTERVAL(this);
}

// ---------------------------------------------------------------- loop

void HeaterUart::loop() {
  service_duty_pulse_();
  expire_pending_command_();
  service_command_tx_();
  read_bus_();
}

// Reads at most MAX_BYTES_PER_LOOP bytes, and stops at the end of a valid
// exchange. Stopping there lets command_slot_fresh_() ask whether more
// bytes wait behind it.
void HeaterUart::read_bus_() {
  const uint32_t loop_start = millis();
  const uint32_t loop_gap = loop_start - last_loop_ms_;
  last_loop_ms_ = loop_start;

  bool exchange_done = false;
  for (size_t n = 0; n < MAX_BYTES_PER_LOOP; n++) {
    uint8_t value = 0;
    if (!available())
      break;
    if (!read_byte(&value))
      break;

    const uint32_t now = millis();
    const bool idle_gap = (now - last_rx_byte_ms_) > IDLE_GAP_MS;
    last_rx_byte_ms_ = now;
    if (skip_echo_byte_(now))
      continue;

    // Process a complete exchange now: the next byte reuses the buffer.
    if (!receiver_.feed(value, idle_gap))
      continue;
    if (!on_exchange_(receiver_.exchange()))
      continue;
    exchange_done = true;
    break;
  }

  if (!exchange_done)
    return;
  if (!command_slot_fresh_(loop_gap))
    return;
  arm_command_(millis());
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

// Returns true when the exchange passed every check.
bool HeaterUart::on_exchange_(const uint8_t *exchange) {
  const p::Check check = p::check_exchange(exchange);
  count_check_(check);
  if (check != p::Check::OK)
    return false;

  if (dump_packets_) {
    ESP_LOGD(TAG, "RX exchange [48 bytes]: %s",
             format_hex_pretty(exchange, p::EXCHANGE_LEN).c_str());
  }

  for (size_t i = 0; i < p::FRAME_LEN; i++)
    controller_frame_[i] = exchange[i];

  telemetry_ = p::decode_exchange(exchange);
  counters_.range_rejects += telemetry_.range_rejects;
  const uint32_t now = millis();
  add_to_window_(now);
  have_telemetry_ = true;
  last_good_ms_ = now;

  // Hold the last documented value when a field is not valid.
  if (telemetry_.run_state.valid)
    run_state_ = telemetry_.run_state.value;
  if (telemetry_.active_error.valid)
    active_error_ = telemetry_.active_error.value;
  if (telemetry_.last_error.valid)
    last_error_ = telemetry_.last_error.value;
  if (telemetry_.demand.valid)
    demand_ = telemetry_.demand.value;

  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    last_exchange_[i] = exchange[i];

  // A change goes out at once. Unchanged values wait for the refresh.
  publish_discrete_(now);
  handle_duty_telemetry_();
  return true;
}

// Adds this exchange's measurements to the window. A gap longer than the
// link timeout starts a fresh window, so samples from before a link loss
// never mix with new ones. Call before last_good_ms_ moves.
void HeaterUart::add_to_window_(uint32_t now) {
  bool fresh = !have_telemetry_;
  if ((now - last_good_ms_) > LINK_TIMEOUT_MS)
    fresh = true;
  if (fresh)
    window_.restart(now);
  if (telemetry_.pump_raw.valid)
    window_.add_pump(telemetry_.pump_raw.value);
  if (telemetry_.fan_rpm.valid)
    window_.add_fan(telemetry_.fan_rpm.value);
  if (telemetry_.chamber_c.valid)
    window_.add_chamber(telemetry_.chamber_c.value);
}

bool HeaterUart::link_up_(uint32_t now) const {
  if (!have_telemetry_)
    return false;
  if ((now - last_good_ms_) > LINK_TIMEOUT_MS)
    return false;
  return true;
}

// ---------------------------------------------------------------- publish

// The gate for every discrete value. Returns true when the value went
// out.
bool HeaterUart::publish_discrete_value_(Discrete d, int32_t value, uint32_t now) {
  sensor::Sensor *target = discrete_[d];
  if (target == nullptr)
    return false;
  if (!gate_[d].due(value, now, DISCRETE_REFRESH_MS))
    return false;
  target->publish_state(static_cast<float>(value));
  gate_[d].mark(value, now);
  return true;
}

// State, power, errors and demand. Called from on_exchange_(), so a
// change goes out at once, and from update(), for the refresh. The texts
// follow their number.
void HeaterUart::publish_discrete_(uint32_t now) {
  if (run_state_ >= 0) {
    if (publish_discrete_value_(D_STATE, run_state_, now))
      publish_text(text_.state_text, heater_state_text(run_state_));
    publish_discrete_value_(D_ON_OFF, p::power_from_run_state(run_state_), now);
  }
  if (last_error_ >= 0) {
    if (publish_discrete_value_(D_ERROR, last_error_, now))
      publish_text(text_.error_text, heater_error_text(last_error_));
  }
  if (active_error_ >= 0)
    publish_discrete_value_(D_ACTIVE_ERROR, active_error_, now);
  if (demand_ >= 0)
    publish_discrete_value_(D_DEMAND, demand_, now);
}

// Sends the window and starts the next one. All four values go out on the
// same update.
void HeaterUart::publish_window_(uint32_t now) {
  if (window_.has_pump()) {
    if (measure_.pump != nullptr)
      measure_.pump->publish_state(static_cast<float>(window_.pump_last()) * 0.1f);
  }
  if (window_.has_fan()) {
    if (measure_.fan != nullptr)
      measure_.fan->publish_state(window_.fan_mean());
  }
  if (window_.has_chamber()) {
    if (measure_.chamber != nullptr)
      measure_.chamber->publish_state(static_cast<float>(window_.chamber_last()));
    if (measure_.chamber_max != nullptr)
      measure_.chamber_max->publish_state(static_cast<float>(window_.chamber_max()));
  }
  window_.restart(now);
}

void HeaterUart::publish_duty_control_() {
  if (duty_control_ == nullptr)
    return;
  if (duty_target_active_)
    return;
  if (duty_stable_value_ < p::limits::DEMAND.min)
    return;
  duty_control_->publish_state(static_cast<float>(duty_stable_value_));
}

// Writes the debug JSON into `out`. Returns false if it does not fit.
bool HeaterUart::format_debug_(char *out, size_t size, uint32_t now) const {
  const p::Telemetry &t = telemetry_;
  float frame_age_s = -1.0f;
  if (have_telemetry_)
    frame_age_s = static_cast<float>(now - last_good_ms_) / 1000.0f;
  float duty = -1.0f;
  if (demand_ >= 0)
    duty = p::duty_from_demand(demand_);
  char hex[p::EXCHANGE_LEN * 2 + 1];
  to_hex(last_exchange_, p::EXCHANGE_LEN, hex);
  const int n = snprintf(
      out, size,
      "{\"frame_age_s\":%.1f,\"frames_ok\":%u,\"bad_start\":%u,\"bad_marker\":%u,"
      "\"bad_crc\":%u,\"range_rejects\":%u,\"duty\":%.1f,"
      "\"raw_ctrl_temperature\":%d,\"raw_pump_min\":%d,\"raw_pump_max\":%d,"
      "\"raw_fan_min\":%d,\"raw_fan_max\":%d,\"raw_ctrl_voltage\":%d,\"raw_mode\":%d,"
      "\"raw_supply_voltage\":%d,\"raw_fan_voltage\":%d,\"raw_glow_voltage\":%d,"
      "\"raw_glow_current\":%d,\"exchange\":\"%s\"}",
      static_cast<double>(frame_age_s), static_cast<unsigned>(counters_.ok),
      static_cast<unsigned>(counters_.bad_start), static_cast<unsigned>(counters_.bad_marker),
      static_cast<unsigned>(counters_.bad_crc), static_cast<unsigned>(counters_.range_rejects),
      static_cast<double>(duty), static_cast<int>(t.ctrl_temperature_raw),
      static_cast<int>(t.ctrl_pump_min_raw), static_cast<int>(t.ctrl_pump_max_raw),
      static_cast<int>(t.ctrl_fan_min_raw), static_cast<int>(t.ctrl_fan_max_raw),
      static_cast<int>(t.ctrl_voltage_raw), static_cast<int>(t.ctrl_mode_raw),
      static_cast<int>(t.supply_voltage_raw), static_cast<int>(t.fan_voltage_raw),
      static_cast<int>(t.glow_voltage_raw), static_cast<int>(t.glow_current_raw), hex);
  if (n < 0)
    return false;
  if (static_cast<size_t>(n) >= size)
    return false;
  return true;
}

// One message with every debug value. Only while debug is on.
void HeaterUart::publish_debug_(uint32_t now) {
  if (!debug_)
    return;
  if (text_.debug_data == nullptr)
    return;
  char json[DEBUG_JSON_SIZE];
  if (!format_debug_(json, sizeof(json), now)) {
    ESP_LOGW(TAG, "Debug data does not fit %u bytes", static_cast<unsigned>(sizeof(json)));
    return;
  }
  text_.debug_data->publish_state(json);
}

// Runs every update_interval. Everything due goes out on this one update.
// With the link down only the link value and debug data go out, so a
// consumer sees the other values age instead of fresh-looking old ones.
void HeaterUart::update() {
  const uint32_t now = millis();
  const bool up = link_up_(now);
  bool came_up = false;
  if (up) {
    if (!link_was_up_)
      came_up = true;
  }
  if (!up) {
    if (link_was_up_)
      link_lost_ = true;
  }
  link_was_up_ = up;

  // After a link loss, every value goes out again at once. At boot the
  // discrete values already went out from on_exchange_().
  if (came_up) {
    if (link_lost_) {
      for (size_t i = 0; i < D_COUNT; i++)
        gate_[i].reset();
      link_lost_ = false;
    }
  }
  publish_discrete_value_(D_LINK, up ? 1 : 0, now);
  publish_debug_(now);
  if (!up)
    return;
  publish_discrete_(now);
  publish_duty_control_();
  // The first window goes out as soon as the link is up, at boot and
  // after a loss. Then once per period.
  if (came_up) {
    publish_window_(now);
    return;
  }
  if (window_.due(now, MEASUREMENT_WINDOW_MS))
    publish_window_(now);
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

// A serial command is armed right after a valid exchange. Skip this slot
// if the loop stalled before it read the exchange: the exchange may have
// ended long ago. Bytes waiting behind the exchange need no check here:
// service_command_tx_() sees them before read_bus_() takes them, and
// drops the slot. A skipped command waits for the next exchange, until
// expire_pending_command_() drops it.
bool HeaterUart::command_slot_fresh_(uint32_t loop_gap_ms) {
  if (pending_command_ == PendingCommand::NONE)
    return false;
  if (loop_gap_ms > COMMAND_MAX_LOOP_GAP_MS) {
    ESP_LOGD(TAG, "Command slot skipped: loop gap %u ms", static_cast<unsigned>(loop_gap_ms));
    return false;
  }
  return true;
}

void HeaterUart::arm_command_(uint32_t now) {
  tx_armed_ = true;
  tx_armed_ms_ = now;
}

// Sends an armed command once the delay has passed on a quiet bus. A byte
// in the meantime means the bus is busy: this slot is lost, and the
// command waits for the next exchange, until it expires.
void HeaterUart::service_command_tx_() {
  if (!tx_armed_)
    return;
  if (pending_command_ == PendingCommand::NONE) {
    tx_armed_ = false;
    return;
  }
  if (available() > 0) {
    tx_armed_ = false;
    ESP_LOGD(TAG, "Command slot lost: the bus spoke during the delay");
    return;
  }
  if ((millis() - tx_armed_ms_) < SERIAL_COMMAND_DELAY_MS)
    return;
  tx_armed_ = false;
  send_pending_command_();
}

// Called only by service_command_tx_(): a command is pending, the bus was
// quiet since the exchange, and controller_frame_ holds the controller
// half of that exchange, which passed every check.
void HeaterUart::send_pending_command_() {
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

  // No flush(): the UART sends from its buffer while loop() goes on. The
  // echo is dropped in read_bus_() as it arrives.
  write_array(command, p::FRAME_LEN);
  echo_left_ = static_cast<uint8_t>(p::FRAME_LEN);
  echo_until_ms_ = millis() + ECHO_WINDOW_MS;
  pending_command_ = PendingCommand::NONE;
  ESP_LOGD(TAG, "Serial command sent");
}

// Drops one byte of our own echo. At most 24 bytes, and only inside the
// echo window. The receiver needs no reset: it reset itself when the
// exchange completed, and the echo never reaches it.
bool HeaterUart::skip_echo_byte_(uint32_t now) {
  if (echo_left_ == 0)
    return false;
  if (static_cast<int32_t>(now - echo_until_ms_) > 0) {
    echo_left_ = 0;
    return false;
  }
  echo_left_--;
  return true;
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
  duty_control_->publish_state(static_cast<float>(target));

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
    duty_control_->publish_state(static_cast<float>(current));
    ESP_LOGD(TAG, "Duty target %d confirmed", current);
    return;
  }

  const bool out_of_rounds = duty_correction_rounds_ >= DUTY_MAX_CORRECTION_ROUNDS;
  const bool out_of_time = (millis() - duty_target_requested_ms_) > DUTY_TARGET_TIMEOUT_MS;
  if (out_of_rounds || out_of_time) {
    ESP_LOGW(TAG, "Duty target %d not reached; controller remains at %d", duty_target_, current);
    duty_target_active_ = false;
    duty_waiting_for_feedback_ = false;
    duty_control_->publish_state(static_cast<float>(current));
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
    duty_control_->publish_state(static_cast<float>(duty_stable_value_));
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

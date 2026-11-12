#pragma once

// Blue-wire protocol of the Chinese diesel heater: framing, CRC, decode.
//
// Pure C++. No ESPHome, no Arduino, no heap. Every function here is
// deterministic and testable on a host build: see tests/host/.
//
// One exchange is 48 bytes: a 24-byte controller frame, then a 24-byte
// heater frame. Each frame starts 0x76 0x16 (0x78 0x16 from a rotary
// controller) and ends in a CRC-16/MODBUS, high byte first.
//
// Byte map. "Confirmed" means seen in a capture from a real heater or
// relied on by the upstream component. "Unverified" means taken from Ray
// Jones' protocol notes and not yet checked against a capture. Unverified
// fields are published raw, without scaling.
//
//   Controller frame, exchange bytes 0..23
//     3      measured temperature, controller sensor      unverified
//     4      demand, 8..35                                confirmed
//     5      pump minimum, 0.1 Hz                         unverified
//     6      pump maximum, 0.1 Hz                         unverified
//     7..8   fan minimum, RPM                             unverified
//     9..10  fan maximum, RPM                             unverified
//     11     system voltage, 0.1 V                        unverified
//     13     operating mode                               confirmed
//   Heater frame, exchange bytes 24..47
//     26     run state, 0..8                              confirmed
//     27     active error state                           confirmed
//     28..29 supply voltage, 0.1 V                        unverified
//     30..31 fan speed, RPM                               confirmed
//     32..33 fan voltage, 0.1 V                           unverified
//     34..35 chamber temperature, degrees C               confirmed
//     36..37 glow plug voltage, 0.1 V                     unverified
//     38..39 glow plug current, 0.01 A                    unverified
//     40     pump frequency, 0.1 Hz                       confirmed
//     41     last error, held until the next start        confirmed
//     44     fixed marker 100                             confirmed
//     45     fixed marker 0                               confirmed

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace heater_uart {
namespace protocol {

constexpr size_t FRAME_LEN = 24;
constexpr size_t EXCHANGE_LEN = 48;
constexpr size_t CRC_DATA_LEN = 22;

constexpr uint8_t START_LCD = 0x76;
constexpr uint8_t START_ROTARY = 0x78;
constexpr uint8_t START_HEATER = 0x76;
constexpr uint8_t SECOND_BYTE = 0x16;
constexpr uint8_t MARKER_44 = 100;
constexpr uint8_t MARKER_45 = 0;

// An undocumented run state some heaters send briefly during start.
// It is not an error. The previous documented state stays.
constexpr uint8_t RUN_STATE_TRANSIENT = 0x10;

// Exchange byte positions.
namespace idx {
constexpr size_t CTRL_TEMPERATURE = 3;
constexpr size_t CTRL_DEMAND = 4;
constexpr size_t CTRL_PUMP_MIN = 5;
constexpr size_t CTRL_PUMP_MAX = 6;
constexpr size_t CTRL_FAN_MIN = 7;
constexpr size_t CTRL_FAN_MAX = 9;
constexpr size_t CTRL_VOLTAGE = 11;
constexpr size_t CTRL_MODE = 13;
constexpr size_t HEATER = FRAME_LEN;
constexpr size_t RUN_STATE = 26;
constexpr size_t ACTIVE_ERROR = 27;
constexpr size_t SUPPLY_VOLTAGE = 28;
constexpr size_t FAN_SPEED = 30;
constexpr size_t FAN_VOLTAGE = 32;
constexpr size_t CHAMBER = 34;
constexpr size_t GLOW_VOLTAGE = 36;
constexpr size_t GLOW_CURRENT = 38;
constexpr size_t PUMP = 40;
constexpr size_t LAST_ERROR = 41;
constexpr size_t MARKER_A = 44;
constexpr size_t MARKER_B = 45;
}  // namespace idx

// Plausibility limits. The CRC already rejects a corrupted exchange, so
// these catch a misread protocol, not line noise. Keep them wide: a limit
// that is too tight hides real data (the old 230 C chamber limit hid
// every overheat).
struct Range {
  int32_t min;
  int32_t max;
};

namespace limits {
constexpr Range RUN_STATE{0, 8};       // documented states only
constexpr Range ERROR_CODE{0, 13};     // documented codes only
constexpr Range DEMAND{8, 35};         // controller range
constexpr Range PUMP_RAW{0, 60};       // 0.0 .. 6.0 Hz
constexpr Range FAN_RPM{0, 8000};      // highest seen 5000
constexpr Range CHAMBER_C{-50, 400};   // body stays under about 300 C
}  // namespace limits

inline bool in_range(int32_t value, Range range) {
  if (value < range.min)
    return false;
  if (value > range.max)
    return false;
  return true;
}

inline uint16_t u16_be(const uint8_t *data, size_t pos) {
  return static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
}

inline int16_t s16_be(const uint8_t *data, size_t pos) {
  return static_cast<int16_t>(u16_be(data, pos));
}

inline uint16_t crc16_modbus(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if ((crc & 1) != 0) {
        crc = static_cast<uint16_t>((crc >> 1) ^ 0xA001);
      } else {
        crc = static_cast<uint16_t>(crc >> 1);
      }
    }
  }
  return crc;
}

inline bool frame_crc_valid(const uint8_t *frame) {
  const uint16_t crc = crc16_modbus(frame, CRC_DATA_LEN);
  if (frame[22] != static_cast<uint8_t>(crc >> 8))
    return false;
  if (frame[23] != static_cast<uint8_t>(crc & 0xFF))
    return false;
  return true;
}

// Writes the CRC into bytes 22 and 23 of a 24-byte frame.
inline void frame_crc_write(uint8_t *frame) {
  const uint16_t crc = crc16_modbus(frame, CRC_DATA_LEN);
  frame[22] = static_cast<uint8_t>(crc >> 8);
  frame[23] = static_cast<uint8_t>(crc & 0xFF);
}

inline bool is_controller_start(uint8_t value) {
  if (value == START_LCD)
    return true;
  if (value == START_ROTARY)
    return true;
  return false;
}

enum class Check : uint8_t {
  OK = 0,
  BAD_START,
  BAD_MARKER,
  BAD_CRC_CONTROLLER,
  BAD_CRC_HEATER,
};

// Checks one complete exchange. Fails on the first bad condition.
inline Check check_exchange(const uint8_t *x) {
  if (!is_controller_start(x[0]))
    return Check::BAD_START;
  if (x[1] != SECOND_BYTE)
    return Check::BAD_START;
  if (x[idx::HEATER] != START_HEATER)
    return Check::BAD_START;
  if (x[idx::HEATER + 1] != SECOND_BYTE)
    return Check::BAD_START;
  if (x[idx::MARKER_A] != MARKER_44)
    return Check::BAD_MARKER;
  if (x[idx::MARKER_B] != MARKER_45)
    return Check::BAD_MARKER;
  if (!frame_crc_valid(x))
    return Check::BAD_CRC_CONTROLLER;
  if (!frame_crc_valid(x + idx::HEATER))
    return Check::BAD_CRC_HEATER;
  return Check::OK;
}

struct Field {
  int32_t value;
  bool valid;
};

// Everything one exchange says. Only decode_exchange() fills it.
struct Telemetry {
  Field run_state;
  Field active_error;
  Field last_error;
  Field pump_raw;
  Field fan_rpm;
  Field chamber_c;
  Field demand;
  // Research fields. Raw values, unverified meaning. Never range checked.
  int32_t ctrl_temperature_raw;
  int32_t ctrl_pump_min_raw;
  int32_t ctrl_pump_max_raw;
  int32_t ctrl_fan_min_raw;
  int32_t ctrl_fan_max_raw;
  int32_t ctrl_voltage_raw;
  int32_t ctrl_mode_raw;
  int32_t supply_voltage_raw;
  int32_t fan_voltage_raw;
  int32_t glow_voltage_raw;
  int32_t glow_current_raw;
  // Fields that failed their limit in this exchange.
  uint8_t range_rejects;
};

inline Field checked_field(int32_t value, Range range, uint8_t *rejects) {
  Field field{value, in_range(value, range)};
  if (!field.valid)
    (*rejects)++;
  return field;
}

// Decodes an exchange that passed check_exchange().
inline Telemetry decode_exchange(const uint8_t *x) {
  Telemetry t{};
  uint8_t rejects = 0;

  const int32_t state = x[idx::RUN_STATE];
  if (state == RUN_STATE_TRANSIENT) {
    t.run_state = Field{state, false};
  } else {
    t.run_state = checked_field(state, limits::RUN_STATE, &rejects);
  }
  t.active_error = checked_field(x[idx::ACTIVE_ERROR], limits::ERROR_CODE, &rejects);
  t.last_error = checked_field(x[idx::LAST_ERROR], limits::ERROR_CODE, &rejects);
  t.pump_raw = checked_field(x[idx::PUMP], limits::PUMP_RAW, &rejects);
  t.fan_rpm = checked_field(u16_be(x, idx::FAN_SPEED), limits::FAN_RPM, &rejects);
  t.chamber_c = checked_field(s16_be(x, idx::CHAMBER), limits::CHAMBER_C, &rejects);
  t.demand = checked_field(x[idx::CTRL_DEMAND], limits::DEMAND, &rejects);

  t.ctrl_temperature_raw = x[idx::CTRL_TEMPERATURE];
  t.ctrl_pump_min_raw = x[idx::CTRL_PUMP_MIN];
  t.ctrl_pump_max_raw = x[idx::CTRL_PUMP_MAX];
  t.ctrl_fan_min_raw = u16_be(x, idx::CTRL_FAN_MIN);
  t.ctrl_fan_max_raw = u16_be(x, idx::CTRL_FAN_MAX);
  t.ctrl_voltage_raw = x[idx::CTRL_VOLTAGE];
  t.ctrl_mode_raw = x[idx::CTRL_MODE];
  t.supply_voltage_raw = u16_be(x, idx::SUPPLY_VOLTAGE);
  t.fan_voltage_raw = u16_be(x, idx::FAN_VOLTAGE);
  t.glow_voltage_raw = u16_be(x, idx::GLOW_VOLTAGE);
  t.glow_current_raw = u16_be(x, idx::GLOW_CURRENT);

  t.range_rejects = rejects;
  return t;
}

// Power is derived from the run state. Byte 27 is an error state, not an
// on/off flag. States 1..5 are starting up or running.
inline int32_t power_from_run_state(int32_t state) {
  if (state < 1)
    return 0;
  if (state > 5)
    return 0;
  return 1;
}

// Duty percentage from the demand byte. In fixed Hz mode the demand does
// not set the pump frequency, so this is not a power level there.
inline float duty_from_demand(int32_t demand) {
  return (static_cast<float>(demand - limits::DEMAND.min) /
          static_cast<float>(limits::DEMAND.max - limits::DEMAND.min)) * 100.0f;
}

// Collects bytes into one exchange. Fixed buffer. No heap.
//
// Sync rule, from the original parser: an exchange starts with a
// controller start byte followed by 0x16. A bus-idle gap always drops a
// partial exchange, so one lost byte cannot shift every later exchange.
class Receiver {
 public:
  // Feeds one byte. Returns true when this byte completes an exchange.
  // idle_gap: true when the bus was idle before this byte.
  bool feed(uint8_t value, bool idle_gap) {
    if (idle_gap)
      reset();
    if (collecting_)
      return collect_(value);
    hunt_(value);
    return false;
  }

  void reset() {
    collecting_ = false;
    have_start_ = false;
    count_ = 0;
  }

  const uint8_t *exchange() const { return buffer_; }

 private:
  bool collect_(uint8_t value) {
    buffer_[count_] = value;
    count_++;
    if (count_ < EXCHANGE_LEN)
      return false;
    reset();
    return true;
  }

  void hunt_(uint8_t value) {
    if (have_start_ && value == SECOND_BYTE) {
      buffer_[1] = value;
      count_ = 2;
      collecting_ = true;
      have_start_ = false;
      return;
    }
    // A start byte can itself begin the next header.
    have_start_ = is_controller_start(value);
    if (have_start_)
      buffer_[0] = value;
  }

  uint8_t buffer_[EXCHANGE_LEN]{};
  size_t count_{0};
  bool collecting_{false};
  bool have_start_{false};
};

}  // namespace protocol
}  // namespace heater_uart
}  // namespace esphome

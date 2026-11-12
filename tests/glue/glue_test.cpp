// Glue tests for components/diesel_heater/heater_uart.cpp.
//
// Builds the real component against a small fake of the ESPHome API, in
// fake/. A manual clock and a fake UART drive it. No device, no ESPHome.
// Run: tests/glue/README.md.
//
// Runs the protocol checks of tests/host/protocol_test.h first, then
// the glue checks. Prints one line per check, then "RESULT pass=N
// fail=M" for each part. Exits 1 on any failure.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "esphome/core/log.h"
#include "heater_uart.h"
#include "protocol_test.h"

namespace p = esphome::heater_uart::protocol;
using esphome::heater_uart::HeaterDutyNumber;
using esphome::heater_uart::HeaterUart;
using esphome::sensor::Sensor;
using esphome::switch_::Switch;
using esphome::text_sensor::TextSensor;
using esphome::uart::UARTComponent;

namespace {

// The same real exchange as tests/host. Heater off. Both CRCs valid.
const uint8_t REAL_EXCHANGE[p::EXCHANGE_LEN] = {
    0x76, 0x16, 0x00, 0x00, 0x0E, 0x0A, 0x2D, 0x05, 0xDC, 0x11, 0x94, 0x78,
    0x01, 0xCD, 0x08, 0x23, 0x05, 0x00, 0xEB, 0x47, 0x0D, 0xAC, 0x6D, 0x7A,
    0x76, 0x16, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x64, 0x00, 0xD4, 0xE0,
};

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const std::string &name) {
  if (ok) {
    g_pass++;
    printf("PASS %s\n", name.c_str());
    return;
  }
  g_fail++;
  printf("FAIL %s\n", name.c_str());
}

void set_clock(uint32_t ms) { esphome::fake::clock_ms() = ms; }
void advance(uint32_t ms) { esphome::fake::clock_ms() += ms; }
uint32_t now() { return esphome::fake::clock_ms(); }

struct Exchange {
  uint8_t b[p::EXCHANGE_LEN];
  Exchange() { memcpy(b, REAL_EXCHANGE, p::EXCHANGE_LEN); }
  // Sets a heater-frame byte and repairs that frame's CRC.
  Exchange &heater_u8(size_t pos, uint8_t value) {
    b[pos] = value;
    p::frame_crc_write(b + p::idx::HEATER);
    return *this;
  }
  Exchange &heater_u16(size_t pos, uint16_t value) {
    b[pos] = static_cast<uint8_t>(value >> 8);
    b[pos + 1] = static_cast<uint8_t>(value & 0xFF);
    p::frame_crc_write(b + p::idx::HEATER);
    return *this;
  }
  Exchange &controller_u8(size_t pos, uint8_t value) {
    b[pos] = value;
    p::frame_crc_write(b);
    return *this;
  }
};

// One component with every optional sensor wired.
struct Rig {
  UARTComponent bus;
  Sensor set_temp, state, error, on_off, pump, fan, chamber, duty;
  Sensor active_error, link, frame_age, frames_ok, bad_start, bad_marker, bad_crc, range_rejects;
  Sensor ctrl_temp, pump_min, pump_max, fan_min, fan_max, ctrl_volt, mode, supply, fan_volt,
      glow_volt, glow_amp;
  TextSensor hex, state_text, error_text;
  Switch up, down;
  std::unique_ptr<HeaterUart> heater;
  std::unique_ptr<HeaterDutyNumber> duty_number;

  explicit Rig(bool with_duty = false) {
    set_clock(1000);
    heater.reset(new HeaterUart(&bus, &set_temp, &state, &error, &on_off, &pump, &fan, &chamber, &duty));
    HeaterUart &h = *heater;
    h.set_active_error_sensor(&active_error);
    h.set_link_sensor(&link);
    h.set_frame_age_sensor(&frame_age);
    h.set_frames_ok_sensor(&frames_ok);
    h.set_frames_bad_start_sensor(&bad_start);
    h.set_frames_bad_marker_sensor(&bad_marker);
    h.set_frames_bad_crc_sensor(&bad_crc);
    h.set_range_rejects_sensor(&range_rejects);
    h.set_ctrl_temperature_raw_sensor(&ctrl_temp);
    h.set_ctrl_pump_min_raw_sensor(&pump_min);
    h.set_ctrl_pump_max_raw_sensor(&pump_max);
    h.set_ctrl_fan_min_raw_sensor(&fan_min);
    h.set_ctrl_fan_max_raw_sensor(&fan_max);
    h.set_ctrl_voltage_raw_sensor(&ctrl_volt);
    h.set_ctrl_mode_raw_sensor(&mode);
    h.set_supply_voltage_raw_sensor(&supply);
    h.set_fan_voltage_raw_sensor(&fan_volt);
    h.set_glow_voltage_raw_sensor(&glow_volt);
    h.set_glow_current_raw_sensor(&glow_amp);
    h.set_exchange_hex_sensor(&hex);
    h.set_state_text_sensor(&state_text);
    h.set_error_text_sensor(&error_text);
    if (with_duty) {
      duty_number.reset(new HeaterDutyNumber(&h));
      h.set_duty_control(duty_number.get(), &up, &down);
    }
    h.setup();
    h.dump_config();
  }

  // A bus-idle gap, then one exchange, then one loop().
  void send(const Exchange &x) {
    advance(200);
    for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
      bus.rx.push_back(x.b[i]);
    heater->loop();
  }
  void send() { send(Exchange()); }
};

// ------------------------------------------------------------ receive

void test_receive_real_exchange() {
  Rig r;
  r.send();
  r.heater->update();
  check(r.frames_ok.state == 1.0f, "receive: one valid exchange counts frames_ok = 1");
  check(r.link.state == 1.0f, "receive: link = 1 after a valid exchange");
  check(r.state.state == 0.0f, "receive: run state 0 published");
  check(r.on_off.state == 0.0f, "receive: on_off 0 published");
  check(r.error.state == 0.0f, "receive: last error 0 published");
  check(r.set_temp.state == 14.0f, "receive: demand 14 published as set_temp");
  check(r.chamber.state == 21.0f, "receive: chamber 21 C published");
  check(r.supply.state == 128.0f, "receive: raw supply voltage 128 published");
  check(r.mode.state == 205.0f, "receive: raw mode 0xCD published");
  check(r.pump_min.state == 10.0f, "receive: raw pump minimum 10 published");
  check(r.fan_max.state == 4500.0f, "receive: raw fan maximum 4500 published");
  check(r.state_text.state == "Off", "receive: state text Off");
  check(r.error_text.state == "No Error", "receive: error text No Error");
  check(r.hex.state.size() == 96, "receive: exchange hex is 96 characters");
  check(r.hex.state.compare(0, 4, "7616") == 0, "receive: exchange hex starts 7616");
}

void test_receive_rejects() {
  Rig r;
  Exchange crc;
  crc.b[p::idx::CHAMBER] ^= 0x01;  // no CRC repair
  r.send(crc);
  Exchange marker;
  marker.heater_u8(p::idx::MARKER_A, 99);
  r.send(marker);
  Exchange start;
  start.b[p::idx::HEATER] = 0x00;
  r.send(start);
  r.heater->update();
  check(r.frames_ok.state == 0.0f, "reject: no exchange accepted");
  check(r.bad_crc.state == 1.0f, "reject: bad CRC counted");
  check(r.bad_marker.state == 1.0f, "reject: bad marker counted");
  check(r.bad_start.state == 1.0f, "reject: bad heater start counted");
  check(r.link.state == 0.0f, "reject: link stays 0");
  check(r.chamber.history.empty(), "reject: chamber never published");
  check(r.state.history.empty(), "reject: state never published");
}

void test_receive_split_across_loops() {
  Rig r;
  advance(200);
  for (size_t i = 0; i < 20; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  advance(5);  // well under the idle gap
  for (size_t i = 20; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  r.heater->update();
  check(r.frames_ok.state == 1.0f, "split: an exchange split over two loops is accepted");
}

void test_receive_idle_gap_drops_partial() {
  Rig r;
  advance(200);
  for (size_t i = 0; i < 20; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  // An idle gap, then a full exchange. Without the gap rule, the 20 old
  // bytes and 28 new ones would form one misframed exchange.
  r.send();
  r.heater->update();
  check(r.frames_ok.state == 1.0f, "gap: partial exchange dropped, the next one accepted");
  check(r.bad_crc.state == 0.0f, "gap: no misframed exchange reaches the CRC check");
}

void test_receive_byte_budget() {
  Rig r;
  advance(200);
  for (int k = 0; k < 4; k++)
    for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
      r.bus.rx.push_back(REAL_EXCHANGE[i]);
  // The 48th byte after an idle gap is the command slot. read_bus_()
  // stops there, also with no command pending.
  r.heater->loop();
  check(r.bus.rx.size() == 3 * p::EXCHANGE_LEN, "budget: the first loop stops at the command slot, byte 48");
  r.heater->loop();
  check(r.bus.rx.size() == 3 * p::EXCHANGE_LEN - 128, "budget: the next loop reads at most 128 bytes");
  r.heater->loop();
  r.heater->update();
  check(r.bus.rx.empty(), "budget: the third loop drains the rest");
  check(r.frames_ok.state == 4.0f, "budget: back-to-back exchanges all accepted");
}

void test_receive_read_failure() {
  Rig r;
  advance(200);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.bus.fail_next_read = true;
  r.heater->loop();
  check(r.bus.rx.size() == p::EXCHANGE_LEN, "read: a failed read_byte() stops the loop, nothing consumed");
  r.heater->loop();
  r.heater->update();
  check(r.frames_ok.state == 1.0f, "read: the next loop reads the exchange");
}

// ------------------------------------------------------------ watchdog

void test_watchdog() {
  Rig r;
  r.heater->update();
  check(r.link.state == 0.0f, "watchdog: link 0 before any exchange");
  check(r.frame_age.history.empty(), "watchdog: no frame_age before any exchange");
  check(r.chamber.history.empty(), "watchdog: no measurement before any exchange");

  r.send();
  const uint32_t t0 = now();
  set_clock(t0 + 10000);
  r.heater->update();
  check(r.link.state == 1.0f, "watchdog: link 1 at exactly 10 s");
  check(r.frame_age.state == 10.0f, "watchdog: frame_age 10 s");
  const size_t chamber_count = r.chamber.history.size();
  const size_t frames_ok_count = r.frames_ok.history.size();

  set_clock(t0 + 10001);
  r.heater->update();
  check(r.link.state == 0.0f, "watchdog: link 0 after 10 s");
  check(r.chamber.history.size() == chamber_count, "watchdog: measurements stop with the link down");
  check(r.frames_ok.history.size() == frames_ok_count + 1, "watchdog: diagnostics keep publishing with the link down");

  r.send();
  r.heater->update();
  check(r.link.state == 1.0f, "watchdog: link back to 1 on the next valid exchange");
  check(r.chamber.history.size() == chamber_count + 1, "watchdog: measurements resume");
}

void test_watchdog_clock_wrap() {
  Rig r;
  set_clock(0xFFFFFFFFu - 3000);
  r.send();
  set_clock(4000);  // millis() wrapped, about 7 s later
  r.heater->update();
  check(r.link.state == 1.0f, "wrap: link survives a millis() wrap");
  set_clock(9000);  // about 12 s later
  r.heater->update();
  check(r.link.state == 0.0f, "wrap: link times out across a millis() wrap");
}

// ------------------------------------------------------------ publish

void test_discrete_on_change() {
  Rig r;
  r.send();
  r.send();
  r.send();
  check(r.state.history.size() == 1, "discrete: same state over 3 exchanges publishes once from loop");
  check(r.state_text.history.size() == 1, "discrete: state text publishes once");
  r.send(Exchange().heater_u8(p::idx::RUN_STATE, 5));
  check(r.state.history.size() == 2, "discrete: a changed state publishes at once");
  check(r.on_off.state == 1.0f, "discrete: state 5 gives on_off 1");
  check(r.state_text.state == "Running Normally", "discrete: state text follows");
  r.heater->update();
  check(r.state.history.size() == 3, "discrete: update() repeats the state");
}

void test_transient_state_held() {
  Rig r;
  r.send(Exchange().heater_u8(p::idx::RUN_STATE, 2));
  r.send(Exchange().heater_u8(p::idx::RUN_STATE, p::RUN_STATE_TRANSIENT));
  r.heater->update();
  check(r.state.state == 2.0f, "transient: state 0x10 holds the previous state");
  check(r.range_rejects.state == 0.0f, "transient: state 0x10 is not a range reject");
}

void test_range_reject_field() {
  Rig r;
  r.send(Exchange().heater_u16(p::idx::CHAMBER, 245));
  r.heater->update();
  check(r.chamber.state == 245.0f, "range: chamber 245 C passes, the old limit was 229");
  r.send(Exchange().heater_u16(p::idx::CHAMBER, 401).heater_u16(p::idx::FAN_SPEED, 3100));
  r.heater->update();
  check(r.chamber.state == 245.0f, "range: chamber 401 C is not published");
  check(r.fan.state == 3100.0f, "range: the other fields of that exchange still publish");
  check(r.range_rejects.state == 1.0f, "range: one range reject counted");
  r.send(Exchange().heater_u16(p::idx::CHAMBER, static_cast<uint16_t>(-5)));
  r.heater->update();
  check(r.chamber.state == -5.0f, "range: chamber -5 C published, signed");
}

void test_error_bytes() {
  Rig r;
  r.send(Exchange().heater_u8(p::idx::ACTIVE_ERROR, 6).heater_u8(p::idx::LAST_ERROR, 6).heater_u8(p::idx::RUN_STATE, 8));
  check(r.active_error.state == 6.0f, "errors: active error 6 published");
  check(r.error.state == 6.0f, "errors: last error 6 published");
  check(r.error_text.state == "Over Temperature", "errors: error text Over Temperature");
  check(r.on_off.state == 0.0f, "errors: on_off 0 in cooldown, not the error byte");
}

void test_hex_rate_limit() {
  Rig r;
  r.send();
  r.heater->update();
  check(r.hex.history.size() == 1, "hex: first exchange published");
  r.send();
  r.heater->update();
  check(r.hex.history.size() == 1, "hex: an unchanged exchange is not published again");
  r.send(Exchange().heater_u16(p::idx::CHAMBER, 30));
  r.heater->update();
  check(r.hex.history.size() == 1, "hex: a change within 30 s waits");
  advance(30000);
  r.send(Exchange().heater_u16(p::idx::CHAMBER, 30));
  r.heater->update();
  check(r.hex.history.size() == 2, "hex: the change publishes after 30 s");
}

void test_null_optional_sensors() {
  set_clock(1000);
  UARTComponent bus;
  Sensor set_temp, state, error, on_off, pump, fan, chamber, duty;
  HeaterUart h(&bus, &set_temp, &state, &error, &on_off, &pump, &fan, &chamber, &duty);
  h.setup();
  advance(200);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    bus.rx.push_back(REAL_EXCHANGE[i]);
  h.loop();
  h.update();
  check(chamber.state == 21.0f, "null: runs with no optional sensor wired");
}

// ------------------------------------------------------------ serial commands

void test_command_sent_after_exchange() {
  Rig r;
  r.send();  // gives a valid controller frame
  r.heater->request_on();
  r.bus.echo_tx = true;
  r.send();
  check(r.bus.tx.size() == p::FRAME_LEN, "command: 24 bytes written after the next exchange");
  bool same = r.bus.tx.size() == p::FRAME_LEN;
  for (size_t i = 0; same && i < p::FRAME_LEN; i++) {
    if (i == 2 || i == 22 || i == 23)
      continue;
    if (r.bus.tx[i] != REAL_EXCHANGE[i])
      same = false;
  }
  check(same, "command: every other byte copies the controller frame");
  check(r.bus.tx.size() > 2 && r.bus.tx[2] == 0xA0, "command: byte 2 is ON 0xA0");
  check(r.bus.tx.size() == p::FRAME_LEN && p::frame_crc_valid(r.bus.tx.data()), "command: CRC valid");
  check(r.bus.rx.empty(), "command: the 24 echo bytes are discarded");
  check(r.bus.flushes == 1, "command: flush() called once");

  r.bus.tx.clear();
  r.send();
  check(r.bus.tx.empty(), "command: sent once only");
}

void test_command_echo_limit() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.bus.echo_tx = true;
  // The heater answers our frame at once: 24 bytes, no idle gap.
  r.bus.reply_after_write.assign(REAL_EXCHANGE + p::idx::HEATER, REAL_EXCHANGE + p::EXCHANGE_LEN);
  r.send();
  check(r.bus.rx.size() == p::FRAME_LEN, "command: discards 24 echo bytes only, the reply stays");
}

// The receiver is out of step at the command slot: one stray byte after
// the idle gap. The last exchange byte and the echo then sit in the FIFO.
// A partial exchange must not complete with echo bytes in it.
void test_command_resets_receiver() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.bus.echo_tx = true;
  advance(200);
  r.bus.rx.push_back(0x00);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  check(r.bus.tx.size() == p::FRAME_LEN, "command: sent at the slot, 48 bytes after the gap");
  advance(1);
  r.heater->loop();
  r.heater->update();
  check(r.bad_crc.state == 0.0f, "command: no partial exchange completes across the transmission");
}

void test_command_off() {
  Rig r;
  r.send();
  r.heater->request_off();
  r.send();
  check(r.bus.tx.size() > 2 && r.bus.tx[2] == 0x05, "command: byte 2 is OFF 0x05");
}

void test_command_needs_frame() {
  Rig r;
  r.heater->request_on();
  Exchange bad;
  bad.b[p::idx::CHAMBER] ^= 0x01;
  r.send(bad);
  check(r.bus.tx.empty(), "command: not sent without a valid controller frame");
}

void test_command_expires() {
  Rig r;
  r.send();
  r.heater->request_on();
  esphome::fake::warn_count() = 0;
  advance(3001);
  r.heater->loop();
  check(esphome::fake::warn_count() == 1, "command: expiry logs one warning");
  r.send();
  check(r.bus.tx.empty(), "command: an expired command is not sent");
}

// ------------------------------------------------------------ duty control

// Counts rising edges on a switch.
size_t presses(const Switch &s) {
  size_t n = 0;
  for (const auto &e : s.edges)
    if (e.on)
      n++;
  return n;
}

// Runs loop() every 10 ms for the given time. Sends the exchange every
// 150 ms, so each exchange follows an idle gap.
void run(Rig &r, uint32_t ms, const Exchange &x) {
  const uint32_t end = now() + ms;
  uint32_t next_exchange = now();
  while (now() < end) {
    if (now() >= next_exchange) {
      for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
        r.bus.rx.push_back(x.b[i]);
      next_exchange = now() + 150;
    }
    r.heater->loop();
    advance(10);
  }
}

void test_duty_reaches_target() {
  Rig r(true);
  Exchange at14;
  r.send(at14);
  r.send(at14);  // two equal samples make the value stable
  r.duty_number->set(17);
  check(r.duty_number->state == 17.0f, "duty: the target publishes at once");
  run(r, 1000, at14);
  check(presses(r.up) == 3, "duty: 3 UP pulses for 14 to 17");
  check(presses(r.down) == 0, "duty: no DOWN pulse");
  bool timing = r.up.edges.size() >= 2 && (r.up.edges[1].at_ms - r.up.edges[0].at_ms) >= 100;
  check(timing, "duty: each pulse is held at least 100 ms");
  check(!r.up.state, "duty: the UP switch ends released");

  Exchange at17;
  at17.controller_u8(p::idx::CTRL_DEMAND, 17);
  run(r, 6000, at17);
  r.heater->update();
  check(presses(r.up) == 3, "duty: no extra pulse once the demand reads 17");
  check(r.duty_number->state == 17.0f, "duty: 17 confirmed");
}

void test_duty_corrects() {
  Rig r(true);
  Exchange at14;
  r.send(at14);
  r.send(at14);
  r.duty_number->set(17);
  run(r, 1000, at14);
  // The controller took only 2 of the 3 presses.
  Exchange at16;
  at16.controller_u8(p::idx::CTRL_DEMAND, 16);
  run(r, 6000, at16);
  check(presses(r.up) == 4, "duty: one corrective UP pulse after feedback");
}

void test_duty_gives_up() {
  Rig r(true);
  Exchange at14;
  r.send(at14);
  r.send(at14);
  esphome::fake::warn_count() = 0;
  r.duty_number->set(20);
  run(r, 40000, at14);  // the controller ignores every press
  check(presses(r.up) == 30, "duty: gives up after 5 rounds of 6 pulses");
  check(esphome::fake::warn_count() == 1, "duty: one warning when it gives up");
  check(r.duty_number->state == 14.0f, "duty: publishes the real value 14 after giving up");
}

void test_duty_clamps() {
  Rig r(true);
  r.send();
  r.send();
  r.duty_number->set(99);
  check(r.duty_number->state == 35.0f, "duty: a target over 35 clamps to 35");
  r.duty_number->set(0);
  check(r.duty_number->state == 8.0f, "duty: a target under 8 clamps to 8");
}

void test_duty_waits_for_stable_value() {
  Rig r(true);
  r.duty_number->set(17);
  run(r, 50, Exchange());  // one sample only: not yet stable
  check(presses(r.up) == 0, "duty: no press before two equal samples");
  run(r, 1000, Exchange());
  check(presses(r.up) == 3, "duty: presses once the value is stable");
}

// ------------------------------------------------------------ random stream

// A seeded xorshift. Same seed, same run.
struct Rng {
  uint32_t s;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
};

// Random bytes, random gaps, valid exchanges mixed in. The sanitizers
// catch memory faults. The checks catch a stuck receiver.
void test_random_stream() {
  Rig r(true);
  Rng rng{0x1234567u};
  for (int round = 0; round < 20000; round++) {
    const uint32_t kind = rng.next() % 4;
    advance(rng.next() % 250);
    if (kind == 0) {
      for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
        r.bus.rx.push_back(REAL_EXCHANGE[i]);
    } else {
      const uint32_t n = rng.next() % 200;
      for (uint32_t i = 0; i < n; i++)
        r.bus.rx.push_back(static_cast<uint8_t>(rng.next()));
    }
    if (rng.next() % 50 == 0)
      r.heater->request_on();
    if (rng.next() % 80 == 0)
      r.duty_number->set(static_cast<float>(rng.next() % 40));
    r.heater->loop();
    r.heater->loop();
    if (rng.next() % 10 == 0)
      r.heater->update();
  }
  r.bus.rx.clear();
  r.send();
  r.send();
  r.heater->update();
  check(r.link.state == 1.0f, "random: after 20000 rounds of noise a clean exchange still links");
  check(r.chamber.state == 21.0f, "random: and decodes");
}

}  // namespace

int main() {
  const int protocol_failures = heater_protocol_test::run_all();
  test_receive_real_exchange();
  test_receive_rejects();
  test_receive_split_across_loops();
  test_receive_idle_gap_drops_partial();
  test_receive_byte_budget();
  test_receive_read_failure();
  test_watchdog();
  test_watchdog_clock_wrap();
  test_discrete_on_change();
  test_transient_state_held();
  test_range_reject_field();
  test_error_bytes();
  test_hex_rate_limit();
  test_null_optional_sensors();
  test_command_sent_after_exchange();
  test_command_echo_limit();
  test_command_resets_receiver();
  test_command_off();
  test_command_needs_frame();
  test_command_expires();
  test_duty_reaches_target();
  test_duty_corrects();
  test_duty_gives_up();
  test_duty_clamps();
  test_duty_waits_for_stable_value();
  test_random_stream();
  printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
  if (protocol_failures != 0)
    return 1;
  if (g_fail != 0)
    return 1;
  return 0;
}

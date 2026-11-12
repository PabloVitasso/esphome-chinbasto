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
#include <cstdlib>
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

const uint32_t MINUTE = 60000;
const uint32_t FIVE_MIN = 300000;

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

// The number after "key": in a flat JSON object, or -99999 if absent.
double json_num(const std::string &json, const char *key) {
  const std::string tag = std::string("\"") + key + "\":";
  const size_t at = json.find(tag);
  if (at == std::string::npos)
    return -99999.0;
  return strtod(json.c_str() + at + tag.size(), nullptr);
}

std::string json_str(const std::string &json, const char *key) {
  const std::string tag = std::string("\"") + key + "\":\"";
  const size_t at = json.find(tag);
  if (at == std::string::npos)
    return "";
  const size_t start = at + tag.size();
  const size_t end = json.find('"', start);
  if (end == std::string::npos)
    return "";
  return json.substr(start, end - start);
}

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

// One component with every sensor wired.
struct Rig {
  UARTComponent bus;
  Sensor state, on_off, error, active_error, demand, link;
  Sensor pump, fan, chamber, chamber_max;
  TextSensor state_text, error_text, debug;
  Switch up, down;
  std::unique_ptr<HeaterUart> heater;
  std::unique_ptr<HeaterDutyNumber> duty_number;

  explicit Rig(bool with_duty = false) {
    set_clock(1000);
    heater.reset(new HeaterUart(&bus));
    HeaterUart &h = *heater;
    h.set_state_sensor(&state);
    h.set_on_off_sensor(&on_off);
    h.set_error_sensor(&error);
    h.set_active_error_sensor(&active_error);
    h.set_demand_sensor(&demand);
    h.set_link_sensor(&link);
    h.set_pump_sensor(&pump);
    h.set_fan_sensor(&fan);
    h.set_chamber_sensor(&chamber);
    h.set_chamber_max_sensor(&chamber_max);
    h.set_state_text_sensor(&state_text);
    h.set_error_text_sensor(&error_text);
    h.set_debug_data_sensor(&debug);
    if (with_duty) {
      duty_number.reset(new HeaterDutyNumber(&h));
      h.set_duty_control(duty_number.get(), &up, &down);
    }
    h.setup();
    h.dump_config();
  }

  // Every message any sensor sent so far.
  size_t messages() const {
    const Sensor *all[] = {&state, &on_off, &error, &active_error, &demand, &link,
                           &pump, &fan, &chamber, &chamber_max};
    size_t n = 0;
    for (const Sensor *s : all)
      n += s->history.size();
    n += state_text.history.size() + error_text.history.size() + debug.history.size();
    return n;
  }

  // Runs loop() every 10 ms on a silent bus, as ESPHome does.
  void idle(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      advance(10);
      heater->loop();
    }
  }

  // A bus-idle gap, then one exchange, then one loop().
  void send(const Exchange &x) {
    idle(200);
    advance(10);
    for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
      bus.rx.push_back(x.b[i]);
    heater->loop();
  }
  void send() { send(Exchange()); }

  // One debug message now, then debug off again.
  std::string debug_now() {
    heater->set_debug(true);
    heater->update();
    heater->set_debug(false);
    return debug.state;
  }
};

// Runs loop() every 10 ms for `ms`, with one exchange every 1 s, and
// update() every 15 s, as the device does. The first update comes 15 s
// after the start, the last one strictly before `ms`.
void live(Rig &r, uint32_t ms, const Exchange &x) {
  const uint32_t end = now() + ms;
  uint32_t next_exchange = now();
  uint32_t next_update = now() + 15000;
  while (now() < end) {
    if (now() >= next_exchange) {
      for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
        r.bus.rx.push_back(x.b[i]);
      next_exchange = now() + 1000;
    }
    r.heater->loop();
    if (now() >= next_update) {
      r.heater->update();
      next_update = now() + 15000;
    }
    advance(10);
  }
}

// ------------------------------------------------------------ receive

void test_receive_real_exchange() {
  Rig r;
  r.send();
  r.heater->update();
  check(r.link.state == 1.0f, "receive: link = 1 after a valid exchange");
  check(r.state.state == 0.0f, "receive: run state 0 published");
  check(r.on_off.state == 0.0f, "receive: on_off 0 published");
  check(r.error.state == 0.0f, "receive: last error 0 published");
  check(r.demand.state == 14.0f, "receive: demand 14 published");
  check(r.chamber.state == 21.0f, "receive: chamber 21 C published when the link comes up");
  check(r.chamber_max.state == 21.0f, "receive: chamber max 21 C published");
  check(r.pump.history.size() == 1, "receive: pump published when the link comes up");
  check(r.state_text.state == "Off", "receive: state text Off");
  check(r.error_text.state == "No Error", "receive: error text No Error");
  const std::string d = r.debug_now();
  check(json_num(d, "frames_ok") == 1.0, "receive: debug frames_ok = 1");
  check(json_num(d, "raw_mode") == 205.0, "receive: debug raw mode 0xCD");
  check(json_num(d, "raw_supply_voltage") == 128.0, "receive: debug raw supply voltage 128");
  check(json_num(d, "raw_pump_min") == 10.0, "receive: debug raw pump minimum 10");
  check(json_num(d, "raw_fan_max") == 4500.0, "receive: debug raw fan maximum 4500");
  check(json_str(d, "exchange").size() == 96, "receive: debug exchange hex is 96 characters");
  check(json_str(d, "exchange").compare(0, 4, "7616") == 0, "receive: debug exchange hex starts 7616");
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
  const std::string d = r.debug_now();
  check(json_num(d, "frames_ok") == 0.0, "reject: no exchange accepted");
  check(json_num(d, "bad_crc") == 1.0, "reject: bad CRC counted");
  check(json_num(d, "bad_marker") == 1.0, "reject: bad marker counted");
  check(json_num(d, "bad_start") == 1.0, "reject: bad heater start counted");
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
  check(json_num(r.debug_now(), "frames_ok") == 1.0, "split: an exchange split over two loops is accepted");
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
  const std::string d = r.debug_now();
  check(json_num(d, "frames_ok") == 1.0, "gap: partial exchange dropped, the next one accepted");
  check(json_num(d, "bad_crc") == 0.0, "gap: no misframed exchange reaches the CRC check");
}

void test_receive_byte_budget() {
  Rig r;
  advance(200);
  for (int k = 0; k < 4; k++)
    for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
      r.bus.rx.push_back(REAL_EXCHANGE[i]);
  // read_bus_() stops at the end of each valid exchange.
  r.heater->loop();
  check(r.bus.rx.size() == 3 * p::EXCHANGE_LEN, "budget: a loop stops at the end of a valid exchange");
  r.heater->loop();
  r.heater->loop();
  r.heater->loop();
  check(r.bus.rx.empty(), "budget: four loops read four back-to-back exchanges");
  check(json_num(r.debug_now(), "frames_ok") == 4.0, "budget: back-to-back exchanges all accepted");

  Rig noise;
  advance(200);
  for (int k = 0; k < 300; k++)
    noise.bus.rx.push_back(0x00);
  noise.heater->loop();
  check(noise.bus.rx.size() == 300 - 128, "budget: a loop reads at most 128 bytes of noise");
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
  check(json_num(r.debug_now(), "frames_ok") == 1.0, "read: the next loop reads the exchange");
}

// ------------------------------------------------------------ watchdog

void test_watchdog() {
  Rig r;
  r.heater->update();
  check(r.link.state == 0.0f, "watchdog: link 0 before any exchange");
  check(r.chamber.history.empty(), "watchdog: no measurement before any exchange");
  check(json_num(r.debug_now(), "frame_age_s") == -1.0, "watchdog: debug frame_age -1 before any exchange");

  r.send();
  const uint32_t t0 = now();
  set_clock(t0 + 10000);
  r.heater->update();
  check(r.link.state == 1.0f, "watchdog: link 1 at exactly 10 s");
  check(json_num(r.debug_now(), "frame_age_s") == 10.0, "watchdog: debug frame_age 10 s");
  const size_t chamber_count = r.chamber.history.size();
  const size_t link_count = r.link.history.size();

  set_clock(t0 + 10001);
  r.heater->update();
  check(r.link.state == 0.0f, "watchdog: link 0 after 10 s");
  check(r.link.history.size() == link_count + 1, "watchdog: the link change goes out at once");
  check(r.chamber.history.size() == chamber_count, "watchdog: measurements stop with the link down");

  const size_t state_count = r.state.history.size();
  r.send();
  r.heater->update();
  check(r.link.state == 1.0f, "watchdog: link back to 1 on the next valid exchange");
  check(r.chamber.history.size() == chamber_count + 1, "watchdog: the window goes out at once after a loss");
  check(r.state.history.size() == state_count + 1, "watchdog: an unchanged state goes out again after a loss");
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

// ------------------------------------------------------------ discrete

void test_discrete_on_change() {
  Rig r;
  r.send();
  r.send();
  r.send();
  check(r.state.history.size() == 1, "discrete: same state over 3 exchanges goes out once");
  check(r.state_text.history.size() == 1, "discrete: state text goes out once");
  r.heater->update();
  check(r.state.history.size() == 1, "discrete: the first update does not repeat it at boot");
  r.send(Exchange().heater_u8(p::idx::RUN_STATE, 5));
  check(r.state.history.size() == 2, "discrete: a changed state goes out at once, from the exchange");
  check(r.on_off.state == 1.0f, "discrete: state 5 gives on_off 1");
  check(r.state_text.state == "Running Normally", "discrete: state text follows");
  r.heater->update();
  check(r.state.history.size() == 2, "discrete: update() does not repeat an unchanged state");
}

void test_discrete_refresh() {
  Rig r;
  r.send();
  r.heater->update();
  const size_t before = r.state.history.size();
  live(r, FIVE_MIN - 20000, Exchange());
  check(r.state.history.size() == before, "refresh: an unchanged state stays quiet under 5 min");
  live(r, 40000, Exchange());
  check(r.state.history.size() == before + 1, "refresh: an unchanged state goes out again after 5 min");
  check(r.state_text.history.size() == 2, "refresh: its text goes out with it");
}

void test_transient_state_held() {
  Rig r;
  r.send(Exchange().heater_u8(p::idx::RUN_STATE, 2));
  r.send(Exchange().heater_u8(p::idx::RUN_STATE, p::RUN_STATE_TRANSIENT));
  r.heater->update();
  check(r.state.state == 2.0f, "transient: state 0x10 holds the previous state");
  check(json_num(r.debug_now(), "range_rejects") == 0.0, "transient: state 0x10 is not a range reject");
}

void test_error_bytes() {
  Rig r;
  r.send(Exchange().heater_u8(p::idx::ACTIVE_ERROR, 6).heater_u8(p::idx::LAST_ERROR, 6).heater_u8(p::idx::RUN_STATE, 8));
  check(r.active_error.state == 6.0f, "errors: active error 6 published");
  check(r.error.state == 6.0f, "errors: last error 6 published");
  check(r.error_text.state == "E-05 Too hot", "errors: error text E-05 Too hot");
  check(r.on_off.state == 0.0f, "errors: on_off 0 in cooldown, not the error byte");
}

// ------------------------------------------------------------ window

Exchange running(uint16_t chamber, uint16_t fan, uint8_t pump_raw) {
  Exchange x;
  x.heater_u8(p::idx::RUN_STATE, 5).heater_u16(p::idx::CHAMBER, chamber).heater_u16(p::idx::FAN_SPEED, fan).heater_u8(p::idx::PUMP, pump_raw);
  return x;
}

void test_window_aggregates() {
  Rig r;
  r.send(running(200, 3000, 25));
  r.heater->update();  // link up: the first window goes out
  const size_t chamber_count = r.chamber.history.size();
  live(r, 20000, running(200, 3000, 25));
  live(r, 5000, running(231, 3100, 25));
  live(r, 20000, running(210, 3200, 27));
  check(r.chamber.history.size() == chamber_count, "window: nothing goes out before 60 s");
  live(r, 20000, running(210, 3200, 27));
  check(r.chamber.history.size() == chamber_count + 1, "window: the window goes out after 60 s");
  check(r.chamber.state == 210.0f, "window: chamber is the last value");
  check(r.chamber_max.state == 231.0f, "window: chamber max keeps the 231 C peak");
  check(r.fan.state > 3000.0f, "window: fan is a mean, above the lowest sample");
  check(r.fan.state < 3200.0f, "window: fan is a mean, below the highest sample");
  bool pump_ok = r.pump.state > 2.69f;
  if (r.pump.state > 2.71f)
    pump_ok = false;
  check(pump_ok, "window: pump is the last value, 2.7 Hz");

  // The window that went out above was sent mid-run, so the next one
  // still holds a few 210 C samples. Two more windows hold only 205 C.
  live(r, 2 * MINUTE + 1000, running(205, 3200, 27));
  check(r.chamber_max.state == 205.0f, "window: a later window starts fresh, the old peak is gone");
}

void test_window_fresh_after_gap() {
  Rig r;
  r.send(running(240, 3000, 25));
  r.heater->update();
  live(r, 30000, running(240, 3000, 25));
  advance(20000);  // the link is lost
  r.heater->update();
  r.send(running(150, 3000, 25));
  r.heater->update();
  check(r.chamber_max.state == 150.0f, "gap: samples from before a link loss do not enter the new window");
}

void test_range_reject_field() {
  Rig r;
  r.send(running(245, 3000, 25));
  r.heater->update();
  check(r.chamber.state == 245.0f, "range: chamber 245 C passes, the old limit was 229");
  live(r, MINUTE + 1000, running(401, 3100, 25));
  check(r.chamber.state == 245.0f, "range: chamber 401 C is never published");
  check(r.fan.state == 3100.0f, "range: the other fields of those exchanges still count");
  check(json_num(r.debug_now(), "range_rejects") >= 1.0, "range: range rejects counted");
  live(r, MINUTE + 1000, running(static_cast<uint16_t>(-5), 3100, 25));
  check(r.chamber.state == -5.0f, "range: chamber -5 C published, signed");
}

// ------------------------------------------------------------ debug and rate

void test_debug_switch() {
  Rig r;
  live(r, MINUTE, Exchange());
  check(r.debug.history.empty(), "debug: nothing goes out while debug is off");
  r.heater->set_debug(true);
  const size_t before = r.debug.history.size();
  live(r, MINUTE + 1000, Exchange());
  check(r.debug.history.size() == before + 4, "debug: one message per update while on, 4 in 61 s");
  r.heater->set_debug(false);
  const size_t after = r.debug.history.size();
  live(r, MINUTE, Exchange());
  check(r.debug.history.size() == after, "debug: nothing again after it is switched off");
  r.heater->set_debug(true);
  advance(20000);  // the link is lost
  r.heater->update();
  check(r.debug.history.size() == after + 1, "debug: it also goes out with the link down");
}

void test_message_rate() {
  Rig r;
  live(r, 30000, running(220, 3300, 27));  // boot, settle
  const size_t before = r.messages();
  live(r, FIVE_MIN, running(220, 3300, 27));
  const size_t sent = r.messages() - before;
  // 4 measurements x 5 windows, plus one refresh of 6 discrete values and
  // 2 texts.
  check(sent <= 30, "rate: steady running sends at most 30 messages in 5 min, got " + std::to_string(sent));
  check(sent >= 20, "rate: steady running still sends the windows, got " + std::to_string(sent));
}

void test_only_required_sensors() {
  set_clock(1000);
  UARTComponent bus;
  Sensor state, on_off, error, pump, fan, chamber;
  HeaterUart h(&bus);
  h.set_state_sensor(&state);
  h.set_on_off_sensor(&on_off);
  h.set_error_sensor(&error);
  h.set_pump_sensor(&pump);
  h.set_fan_sensor(&fan);
  h.set_chamber_sensor(&chamber);
  h.setup();
  h.set_debug(true);
  advance(200);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    bus.rx.push_back(REAL_EXCHANGE[i]);
  h.loop();
  h.update();
  check(chamber.state == 21.0f, "null: runs with only the required sensors wired, debug on");
}

// ============================================================
// 1. Realistic scenarios. One real situation each, with the heater's
//    real sequence of states and values.
// ============================================================

// One heater snapshot as an exchange.
struct Snap {
  uint8_t state;
  int16_t chamber;
  uint16_t fan;
  uint8_t pump_raw;  // 0.1 Hz
  uint8_t last_error;
  uint8_t active_error;
};

Exchange snap(const Snap &v) {
  Exchange x;
  x.heater_u8(p::idx::RUN_STATE, v.state)
      .heater_u16(p::idx::CHAMBER, static_cast<uint16_t>(v.chamber))
      .heater_u16(p::idx::FAN_SPEED, v.fan)
      .heater_u8(p::idx::PUMP, v.pump_raw)
      .heater_u8(p::idx::LAST_ERROR, v.last_error)
      .heater_u8(p::idx::ACTIVE_ERROR, v.active_error);
  return x;
}

// Collapses repeats: 0,0,1,1,2 -> 0,1,2.
std::vector<float> distinct(const std::vector<float> &h) {
  std::vector<float> out;
  for (float v : h) {
    if (!out.empty()) {
      if (out.back() == v)
        continue;
    }
    out.push_back(v);
  }
  return out;
}

void scenario_cold_start() {
  Rig r;
  live(r, 2 * MINUTE, snap({0, 12, 0, 0, 0, 0}));
  const size_t start = r.messages();
  r.send(snap({1, 12, 1500, 0, 0, 1}));
  check(r.state.state == 1.0f, "cold start: state 1 goes out with the exchange, before any update");
  check(r.on_off.state == 1.0f, "cold start: on_off turns 1 at state 1");
  live(r, 30000, snap({1, 12, 1500, 0, 0, 1}));
  live(r, 2 * MINUTE, snap({2, 20, 2000, 0, 0, 1}));
  live(r, MINUTE, snap({4, 60, 2500, 15, 0, 1}));
  live(r, MINUTE, snap({4, 120, 2700, 15, 0, 1}));
  live(r, 5 * MINUTE, snap({5, 200, 3100, 25, 0, 1}));
  live(r, 5 * MINUTE, snap({5, 215, 3125, 25, 0, 1}));
  const std::vector<float> seq = distinct(r.state.history);
  const std::vector<float> want = {0, 1, 2, 4, 5};
  check(seq == want, "cold start: the states go out in order 0, 1, 2, 4, 5");
  check(r.state_text.state == "Running Normally", "cold start: the state text ends Running Normally");
  check(r.chamber.state == 215.0f, "cold start: chamber settles at 215 C");
  check(r.chamber_max.state >= r.chamber.state, "cold start: chamber max is never below the last value");
  bool pump_ok = r.pump.state > 2.49f;
  if (r.pump.state > 2.51f)
    pump_ok = false;
  check(pump_ok, "cold start: pump 2.5 Hz");
  const size_t sent = r.messages() - start;
  check(sent <= 110, "cold start: 15 min of start-up send at most 110 messages, got " + std::to_string(sent));
}

void scenario_e05_trip() {
  Rig r;
  live(r, 3 * MINUTE, snap({5, 220, 3125, 25, 0, 1}));
  live(r, 20000, snap({5, 235, 3125, 25, 0, 1}));
  live(r, 5000, snap({5, 250, 3125, 25, 0, 1}));
  r.send(snap({8, 245, 3000, 0, 6, 6}));
  check(r.error.state == 6.0f, "E-05: error 6 goes out with the trip exchange, before any update");
  check(r.error_text.state == "E-05 Too hot", "E-05: the error text is E-05 Too hot");
  check(r.active_error.state == 6.0f, "E-05: active error 6");
  check(r.on_off.state == 0.0f, "E-05: on_off 0 in cooldown");
  check(r.state_text.state == "Cooldown", "E-05: state text Cooldown");
  live(r, 75000, snap({8, 230, 2900, 0, 6, 6}));
  // The chamber maximum of a window that held the 250 C peak.
  bool peak_seen = false;
  for (float v : r.chamber_max.history) {
    if (v == 250.0f)
      peak_seen = true;
  }
  check(peak_seen, "E-05: a window reports the 250 C peak, though no update saw it");
  check(r.chamber.state == 230.0f, "E-05: chamber last value follows the cooldown");
}

void scenario_restart_after_trip() {
  Rig r;
  live(r, 2 * MINUTE, snap({0, 40, 0, 0, 6, 6}));
  check(r.error.state == 6.0f, "restart: the held error 6 shows while off");
  r.send(snap({1, 40, 1500, 0, 0, 1}));
  check(r.error.state == 0.0f, "restart: the start clears error 6 at once");
  check(r.error_text.state == "No Error", "restart: the error text returns to No Error");
  check(r.active_error.state == 1.0f, "restart: active error state 1, running");
}

void scenario_link_loss_mid_run() {
  Rig r;
  live(r, 3 * MINUTE, snap({5, 218, 3125, 25, 0, 1}));
  const size_t chamber_before = r.chamber.history.size();
  const size_t state_before = r.state.history.size();
  // The ESP hears nothing for 40 s: a brownout, a loose wire.
  for (int i = 0; i < 3; i++) {
    r.idle(15000);
    r.heater->update();
  }
  check(r.link.state == 0.0f, "link loss: link 0 goes out within one update after 10 s");
  check(r.chamber.history.size() == chamber_before, "link loss: no measurement goes out while the link is down");
  r.send(snap({5, 190, 3125, 25, 0, 1}));
  r.heater->update();
  check(r.link.state == 1.0f, "link loss: link 1 after the next valid exchange");
  check(r.chamber_max.state == 190.0f, "link loss: the new window holds only samples after the loss");
  check(r.state.history.size() == state_before + 1, "link loss: the unchanged state goes out again");
}

void scenario_panel_hz_change() {
  Rig r;
  live(r, 3 * MINUTE, snap({5, 218, 3125, 25, 0, 1}));
  live(r, 75000, snap({5, 216, 3620, 29, 0, 1}));
  bool pump_ok = r.pump.state > 2.89f;
  if (r.pump.state > 2.91f)
    pump_ok = false;
  check(pump_ok, "Hz change: the new 2.9 Hz shows within 75 s");
  check(r.fan.state > 3125.0f, "Hz change: the fan mean rises with the Hz");
}

void scenario_debug_session() {
  Rig r;
  live(r, MINUTE, snap({5, 218, 3125, 25, 0, 1}));
  r.heater->set_debug(true);
  live(r, 31000, snap({5, 218, 3125, 25, 0, 1}));
  r.heater->set_debug(false);
  const size_t n = r.debug.history.size();
  check(n == 2, "debug session: two updates, two JSON messages, got " + std::to_string(n));
  if (n < 2)
    return;
  const double a = json_num(r.debug.history[n - 2], "frames_ok");
  const double b = json_num(r.debug.history[n - 1], "frames_ok");
  check(b - a == 15.0, "debug session: frames_ok rises by 15 between updates, one exchange per second");
  check(json_num(r.debug.history[n - 1], "bad_crc") == 0.0, "debug session: a clean bus has no CRC errors");
}

void scenario_night_off() {
  Rig r;
  live(r, MINUTE, snap({0, 8, 0, 0, 0, 0}));
  const size_t start = r.messages();
  live(r, 8 * 60 * MINUTE, snap({0, 8, 0, 0, 0, 0}));
  const size_t sent = r.messages() - start;
  // 4 measurements a minute, plus 8 discrete values and texts every 5 min.
  check(sent <= 8 * 60 * 6, "night off: 8 h send at most 6 messages a minute, got " + std::to_string(sent));
  check(r.debug.history.empty(), "night off: no debug data with the switch off");
}

// ============================================================
// 2. Valid boundaries. The edges that must still pass.
// ============================================================

void boundary_valid_fields() {
  Rig r;
  r.send(snap({5, 400, 8000, 60, 13, 13}));
  r.heater->update();
  check(r.chamber.state == 400.0f, "valid edge: chamber 400 C passes");
  check(r.fan.state == 8000.0f, "valid edge: fan 8000 RPM passes");
  bool pump_ok = r.pump.state > 5.99f;
  if (r.pump.state > 6.01f)
    pump_ok = false;
  check(pump_ok, "valid edge: pump 6.0 Hz passes");
  check(r.error.state == 13.0f, "valid edge: error 13 passes");
  live(r, MINUTE + 1000, snap({8, -50, 0, 0, 0, 0}));
  check(r.chamber.state == -50.0f, "valid edge: chamber -50 C passes");
  check(r.state.state == 8.0f, "valid edge: state 8 passes");
  r.send(Exchange().controller_u8(p::idx::CTRL_DEMAND, 8));
  check(r.demand.state == 8.0f, "valid edge: demand 8 passes");
  r.send(Exchange().controller_u8(p::idx::CTRL_DEMAND, 35));
  check(r.demand.state == 35.0f, "valid edge: demand 35 passes");
  check(json_num(r.debug_now(), "range_rejects") == 0.0, "valid edge: no range reject on any edge value");
}

void boundary_policy_timing() {
  using esphome::heater_uart::policy::ChangeGate;
  using esphome::heater_uart::policy::MinuteWindow;
  ChangeGate g;
  check(g.due(5, 0, FIVE_MIN), "policy edge: the first value is always due");
  g.mark(5, 1000);
  check(!g.due(5, 1000 + FIVE_MIN - 1, FIVE_MIN), "policy edge: unchanged, 1 ms before the refresh: not due");
  check(g.due(5, 1000 + FIVE_MIN, FIVE_MIN), "policy edge: unchanged, at exactly the refresh: due");
  check(g.due(6, 1001, FIVE_MIN), "policy edge: a change of 1 is due at once");
  g.reset();
  check(g.due(5, 1001, FIVE_MIN), "policy edge: after reset() an unchanged value is due");
  g.mark(5, 0xFFFFFFFFu - 1000);
  check(!g.due(5, 1000, FIVE_MIN), "policy edge: the refresh survives a millis() wrap");

  MinuteWindow w;
  check(!w.due(0, MINUTE), "policy edge: a window never started is never due");
  w.restart(1000);
  check(!w.due(1000 + MINUTE - 1, MINUTE), "policy edge: window 1 ms before 60 s: not due");
  check(w.due(1000 + MINUTE, MINUTE), "policy edge: window at exactly 60 s: due");
  check(!w.has_fan(), "policy edge: a fresh window has no fan sample");
  check(w.fan_mean() == 0.0f, "policy edge: fan mean with no sample is 0, no division by zero");
  for (uint32_t i = 0; i < MinuteWindow::MAX_SAMPLES + 10; i++)
    w.add_fan(8000);
  check(w.fan_mean() == 8000.0f, "policy edge: the sample cap keeps the mean exact at 8000 RPM");
  w.add_chamber(-50);
  w.add_chamber(-60);
  check(w.chamber_max() == -50, "policy edge: chamber max works below zero");
}

// ============================================================
// 3. Invalid boundaries. One step past each edge.
// ============================================================

void boundary_invalid_fields() {
  Rig r;
  r.send(snap({5, 200, 3000, 25, 0, 1}));
  r.heater->update();
  const size_t chamber_n = r.chamber.history.size();
  live(r, MINUTE + 1000, snap({5, 401, 8001, 61, 0, 1}));
  check(r.chamber.history.size() == chamber_n, "invalid edge: chamber 401, fan 8001, pump 6.1 Hz: no measurement goes out");
  live(r, MINUTE + 1000, snap({5, -51, 3000, 25, 0, 1}));
  check(r.chamber.state == 200.0f, "invalid edge: chamber -51 C is never published");
  r.send(snap({9, 200, 3000, 25, 14, 14}));
  check(r.state.state == 5.0f, "invalid edge: state 9 holds the last state 5");
  check(r.error.state == 0.0f, "invalid edge: error 14 holds the last error 0");
  r.send(Exchange().controller_u8(p::idx::CTRL_DEMAND, 7));
  r.send(Exchange().controller_u8(p::idx::CTRL_DEMAND, 36));
  check(r.demand.state == 14.0f, "invalid edge: demand 7 and 36 hold the last demand 14");
  check(json_num(r.debug_now(), "range_rejects") > 0.0, "invalid edge: every reject is counted");
}

// ============================================================
// 4. Structural mutations: framing, CRC, timing of bytes. The receive
//    tests above, and the serial command tests below.
// 5. Randomized fuzzing: test_random_stream, last.
// ============================================================

// ------------------------------------------------------------ serial commands

void test_command_sent_after_exchange() {
  Rig r;
  r.send();  // gives a valid controller frame
  r.heater->request_on();
  r.bus.echo_tx = true;
  r.send();
  check(r.bus.tx.empty(), "command: armed after the exchange, not sent in the same loop");
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "command: 24 bytes written 50 ms after the exchange");
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
  check(r.bus.rx.empty(), "command: the 24 echo bytes are read and dropped");
  check(json_num(r.debug_now(), "bad_start") == 0.0, "command: the echo never reaches the receiver");
  check(r.bus.flushes == 0, "command: no flush(), the loop never waits for the UART");

  r.bus.tx.clear();
  r.send();
  r.idle(60);
  check(r.bus.tx.empty(), "command: sent once only");
}

// The heater answers at once with a full valid exchange, right behind
// our echo. Exactly 24 echo bytes are dropped: one more and the answer
// would fail its checks.
void test_command_echo_limit() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.bus.echo_tx = true;
  r.bus.reply_after_write.assign(REAL_EXCHANGE, REAL_EXCHANGE + p::EXCHANGE_LEN);
  r.send();
  r.idle(60);
  const std::string d = r.debug_now();
  check(json_num(d, "frames_ok") == 3.0, "command: drops exactly the 24 echo bytes, the answer behind them is accepted");
  check(json_num(d, "bad_start") == 0.0, "command: no framing error from the echo");
}

// No echo comes back, for example with separate RX and TX pins. After the
// echo window the bus is read normally again.
void test_command_echo_window_ends() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.send();
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "echo window: the command went out");
  r.send();
  check(json_num(r.debug_now(), "frames_ok") == 3.0, "echo window: an exchange after the window is not dropped");
}

// The arming loop returns at once. The clock does not move inside it.
void test_command_never_blocks() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.idle(200);
  advance(10);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  const uint32_t before = now();
  r.heater->loop();
  check(now() == before, "no block: the loop that arms the command does not wait");
  r.idle(40);
  check(r.bus.tx.empty(), "no block: nothing is sent 40 ms after the exchange");
  r.idle(20);
  check(r.bus.tx.size() == p::FRAME_LEN, "no block: sent once 50 ms have passed");
}

// The panel speaks during the 50 ms. Sending now would collide, so the
// slot is lost and the command waits for the next exchange.
void test_command_lost_when_bus_speaks() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.send();
  r.idle(20);
  for (size_t i = 0; i < 5; i++)
    r.bus.rx.push_back(0x55);
  r.idle(60);
  check(r.bus.tx.empty(), "bus busy: no command while the bus spoke during the delay");
  r.send();
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "bus busy: sent at the next quiet exchange");
}

// One stray byte after the idle gap. The exchange then ends at byte 49.
// The command follows the valid exchange, not a byte count.
void test_command_after_stray_byte() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.idle(200);
  advance(10);
  r.bus.rx.push_back(0x00);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "command: sent after a valid exchange that starts one byte late");
}

// Bytes already wait behind the exchange: the bus has moved on, so a
// frame now would be late. The command waits for the next exchange.
void test_command_skips_when_bytes_wait() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.idle(200);
  advance(10);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  for (size_t i = 0; i < 10; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  check(r.bus.tx.empty(), "command: skipped while bytes wait behind the exchange");
  r.bus.rx.clear();
  r.send();
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "command: sent at the next fresh exchange");
}

// The loop stalled before it read the exchange. The exchange may have
// ended long ago. The command waits for the next exchange.
void test_command_skips_after_stall() {
  Rig r;
  r.send();
  r.heater->request_on();
  r.idle(200);
  advance(31);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    r.bus.rx.push_back(REAL_EXCHANGE[i]);
  r.heater->loop();
  r.idle(60);
  check(r.bus.tx.empty(), "command: skipped after a loop gap of 31 ms");
  r.send();
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "command: sent at the next fresh exchange after a stall");

  Rig edge;
  edge.send();
  edge.heater->request_on();
  edge.idle(200);
  advance(30);
  for (size_t i = 0; i < p::EXCHANGE_LEN; i++)
    edge.bus.rx.push_back(REAL_EXCHANGE[i]);
  edge.heater->loop();
  edge.idle(60);
  check(edge.bus.tx.size() == p::FRAME_LEN, "command: sent after a loop gap of exactly 30 ms");
}

void test_command_off() {
  Rig r;
  r.send();
  r.heater->request_off();
  r.send();
  r.idle(60);
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

// A valid controller frame exists, so only the exchange check can stop
// the command.
void test_command_needs_valid_exchange() {
  Rig r;
  r.send();
  r.heater->request_on();
  Exchange bad;
  bad.b[p::idx::CHAMBER] ^= 0x01;
  r.send(bad);
  r.idle(60);
  check(r.bus.tx.empty(), "command: not sent after an exchange that fails its CRC");
  r.send();
  r.idle(60);
  check(r.bus.tx.size() == p::FRAME_LEN, "command: sent at the next valid exchange");
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
    if (rng.next() % 200 == 0)
      r.heater->set_debug(!r.heater->get_debug());
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
  // 1. Realistic scenarios.
  scenario_cold_start();
  scenario_e05_trip();
  scenario_restart_after_trip();
  scenario_link_loss_mid_run();
  scenario_panel_hz_change();
  scenario_debug_session();
  scenario_night_off();
  // 2. Valid boundaries.
  boundary_valid_fields();
  boundary_policy_timing();
  // 3. Invalid boundaries.
  boundary_invalid_fields();
  // 4. Structure: receive, discrete, window, commands, duty.
  test_receive_real_exchange();
  test_receive_rejects();
  test_receive_split_across_loops();
  test_receive_idle_gap_drops_partial();
  test_receive_byte_budget();
  test_receive_read_failure();
  test_watchdog();
  test_watchdog_clock_wrap();
  test_discrete_on_change();
  test_discrete_refresh();
  test_transient_state_held();
  test_error_bytes();
  test_window_aggregates();
  test_window_fresh_after_gap();
  test_range_reject_field();
  test_debug_switch();
  test_message_rate();
  test_only_required_sensors();
  test_command_sent_after_exchange();
  test_command_echo_limit();
  test_command_echo_window_ends();
  test_command_never_blocks();
  test_command_lost_when_bus_speaks();
  test_command_after_stray_byte();
  test_command_skips_when_bytes_wait();
  test_command_skips_after_stall();
  test_command_off();
  test_command_needs_frame();
  test_command_needs_valid_exchange();
  test_command_expires();
  test_duty_reaches_target();
  test_duty_corrects();
  test_duty_gives_up();
  test_duty_clamps();
  test_duty_waits_for_stable_value();
  // 5. Randomized fuzzing.
  test_random_stream();
  printf("RESULT pass=%d fail=%d\n", g_pass, g_fail);
  if (protocol_failures != 0)
    return 1;
  if (g_fail != 0)
    return 1;
  return 0;
}

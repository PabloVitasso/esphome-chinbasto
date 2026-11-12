#pragma once

// Host tests for heater_protocol.h. Run: tests/host/README.md.
// Prints one line per check, then "RESULT pass=N fail=M".

#include <cstdio>
#include <cstring>

#include "heater_protocol.h"

namespace heater_protocol_test {

namespace p = esphome::heater_uart::protocol;

// A real exchange, captured from the heater in vaniverse in 2025.
// Heater off. Both CRCs valid.
static const uint8_t REAL_EXCHANGE[p::EXCHANGE_LEN] = {
    0x76, 0x16, 0x00, 0x00, 0x0E, 0x0A, 0x2D, 0x05, 0xDC, 0x11, 0x94, 0x78,
    0x01, 0xCD, 0x08, 0x23, 0x05, 0x00, 0xEB, 0x47, 0x0D, 0xAC, 0x6D, 0x7A,
    0x76, 0x16, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x64, 0x00, 0xD4, 0xE0,
};

struct Tally {
  int pass = 0;
  int fail = 0;
};

static void check(Tally *t, bool ok, const char *name) {
  if (ok) {
    t->pass++;
    printf("PASS %s\n", name);
    return;
  }
  t->fail++;
  printf("FAIL %s\n", name);
}

static void copy_real(uint8_t *x) { memcpy(x, REAL_EXCHANGE, p::EXCHANGE_LEN); }

// Sets a heater-frame field, then repairs that frame's CRC.
static void set_heater_u16(uint8_t *x, size_t pos, uint16_t value) {
  x[pos] = static_cast<uint8_t>(value >> 8);
  x[pos + 1] = static_cast<uint8_t>(value & 0xFF);
  p::frame_crc_write(x + p::idx::HEATER);
}

static void set_heater_u8(uint8_t *x, size_t pos, uint8_t value) {
  x[pos] = value;
  p::frame_crc_write(x + p::idx::HEATER);
}

static void test_crc(Tally *t) {
  check(t, p::crc16_modbus(REAL_EXCHANGE + 24, 22) == 0xD4E0, "crc heater frame = D4E0");
  check(t, p::crc16_modbus(REAL_EXCHANGE, 22) == 0x6D7A, "crc controller frame = 6D7A");
  check(t, p::check_exchange(REAL_EXCHANGE) == p::Check::OK, "real exchange passes");
}

static void test_rejects(Tally *t) {
  uint8_t x[p::EXCHANGE_LEN];
  copy_real(x);
  x[35] ^= 0x01;
  check(t, p::check_exchange(x) == p::Check::BAD_CRC_HEATER, "flipped heater bit -> bad crc heater");
  copy_real(x);
  x[4] ^= 0x01;
  check(t, p::check_exchange(x) == p::Check::BAD_CRC_CONTROLLER, "flipped controller bit -> bad crc controller");
  copy_real(x);
  x[0] = 0x55;
  check(t, p::check_exchange(x) == p::Check::BAD_START, "wrong start -> bad start");
  copy_real(x);
  x[24] = 0x78;
  check(t, p::check_exchange(x) == p::Check::BAD_START, "rotary start on heater frame -> bad start");
  copy_real(x);
  x[44] = 99;
  check(t, p::check_exchange(x) == p::Check::BAD_MARKER, "marker 44 wrong -> bad marker");
  copy_real(x);
  x[0] = 0x78;
  p::frame_crc_write(x);
  check(t, p::check_exchange(x) == p::Check::OK, "rotary controller start accepted");
}

static void test_decode_real(Tally *t) {
  const p::Telemetry d = p::decode_exchange(REAL_EXCHANGE);
  check(t, d.run_state.valid && d.run_state.value == 0, "run state 0");
  check(t, d.active_error.valid && d.active_error.value == 0, "active error 0");
  check(t, d.last_error.valid && d.last_error.value == 0, "last error 0");
  check(t, d.chamber_c.valid && d.chamber_c.value == 21, "chamber 21 C");
  check(t, d.fan_rpm.valid && d.fan_rpm.value == 0, "fan 0");
  check(t, d.pump_raw.valid && d.pump_raw.value == 0, "pump 0");
  check(t, d.demand.valid && d.demand.value == 14, "demand 14");
  check(t, d.ctrl_pump_min_raw == 10, "pump min raw 10");
  check(t, d.ctrl_pump_max_raw == 45, "pump max raw 45");
  check(t, d.ctrl_fan_min_raw == 1500, "fan min raw 1500");
  check(t, d.ctrl_fan_max_raw == 4500, "fan max raw 4500");
  check(t, d.ctrl_voltage_raw == 120, "voltage raw 120");
  check(t, d.ctrl_mode_raw == 0xCD, "mode raw 0xCD");
  check(t, d.supply_voltage_raw == 128, "supply voltage raw 128");
  check(t, d.range_rejects == 0, "no range rejects");
}

static void test_limits(Tally *t) {
  uint8_t x[p::EXCHANGE_LEN];
  copy_real(x);
  set_heater_u16(x, p::idx::CHAMBER, 245);
  p::Telemetry d = p::decode_exchange(x);
  check(t, d.chamber_c.valid && d.chamber_c.value == 245, "chamber 245 C published (old limit 230)");

  copy_real(x);
  set_heater_u16(x, p::idx::CHAMBER, static_cast<uint16_t>(-5));
  d = p::decode_exchange(x);
  check(t, d.chamber_c.valid && d.chamber_c.value == -5, "chamber -5 C published (old limit > 0)");

  copy_real(x);
  set_heater_u16(x, p::idx::CHAMBER, 401);
  d = p::decode_exchange(x);
  check(t, !d.chamber_c.valid && d.range_rejects == 1, "chamber 401 C rejected and counted");

  copy_real(x);
  set_heater_u16(x, p::idx::FAN_SPEED, 5200);
  d = p::decode_exchange(x);
  check(t, d.fan_rpm.valid && d.fan_rpm.value == 5200, "fan 5200 published (old limit 5000)");

  copy_real(x);
  set_heater_u16(x, p::idx::FAN_SPEED, 800);
  d = p::decode_exchange(x);
  check(t, d.fan_rpm.valid, "fan 800 published during a ramp (old limit 1000)");

  copy_real(x);
  set_heater_u16(x, p::idx::FAN_SPEED, 9000);
  d = p::decode_exchange(x);
  check(t, !d.fan_rpm.valid && d.range_rejects == 1, "fan 9000 rejected and counted");

  copy_real(x);
  set_heater_u8(x, p::idx::PUMP, 8);
  d = p::decode_exchange(x);
  check(t, d.pump_raw.valid && d.pump_raw.value == 8, "pump 0.8 Hz published (old limit 1.2)");

  copy_real(x);
  set_heater_u8(x, p::idx::RUN_STATE, p::RUN_STATE_TRANSIENT);
  d = p::decode_exchange(x);
  check(t, !d.run_state.valid && d.range_rejects == 0, "state 0x10 held, not counted");

  copy_real(x);
  set_heater_u8(x, p::idx::RUN_STATE, 9);
  d = p::decode_exchange(x);
  check(t, !d.run_state.valid && d.range_rejects == 1, "state 9 rejected and counted");

  copy_real(x);
  set_heater_u8(x, p::idx::ACTIVE_ERROR, 6);
  d = p::decode_exchange(x);
  check(t, d.active_error.valid && d.active_error.value == 6, "active error 6 (E-05)");
}

static void test_helpers(Tally *t) {
  check(t, p::power_from_run_state(0) == 0, "power state 0 -> 0");
  check(t, p::power_from_run_state(1) == 1, "power state 1 -> 1");
  check(t, p::power_from_run_state(5) == 1, "power state 5 -> 1");
  check(t, p::power_from_run_state(6) == 0, "power state 6 -> 0");
  check(t, p::duty_from_demand(8) == 0.0f, "duty demand 8 -> 0");
  check(t, p::duty_from_demand(35) == 100.0f, "duty demand 35 -> 100");
}

// Feeds bytes. Returns how many exchanges completed. Copies the last one.
static int feed(p::Receiver *r, const uint8_t *bytes, size_t n, bool gap_first, uint8_t *last) {
  int done = 0;
  for (size_t i = 0; i < n; i++) {
    const bool gap = gap_first && i == 0;
    if (!r->feed(bytes[i], gap))
      continue;
    done++;
    memcpy(last, r->exchange(), p::EXCHANGE_LEN);
  }
  return done;
}

static void test_receiver(Tally *t) {
  uint8_t last[p::EXCHANGE_LEN];
  p::Receiver r;

  int done = feed(&r, REAL_EXCHANGE, p::EXCHANGE_LEN, true, last);
  check(t, done == 1, "receiver: one exchange completes");
  check(t, memcmp(last, REAL_EXCHANGE, p::EXCHANGE_LEN) == 0, "receiver: bytes intact");

  r.reset();
  const uint8_t noise[] = {0x00, 0x16, 0x76, 0x00, 0x16, 0x99, 0x78};
  done = feed(&r, noise, sizeof(noise), true, last);
  done += feed(&r, REAL_EXCHANGE, p::EXCHANGE_LEN, true, last);
  check(t, done == 1, "receiver: noise then gap then exchange -> one");
  check(t, memcmp(last, REAL_EXCHANGE, p::EXCHANGE_LEN) == 0, "receiver: bytes intact after noise");

  // A gap inside an exchange drops the partial one.
  r.reset();
  done = feed(&r, REAL_EXCHANGE, 20, true, last);
  done += feed(&r, REAL_EXCHANGE + 20, p::EXCHANGE_LEN - 20, true, last);
  check(t, done == 0, "receiver: gap mid-exchange drops it");

  // Back to back, no gap.
  r.reset();
  done = feed(&r, REAL_EXCHANGE, p::EXCHANGE_LEN, true, last);
  done += feed(&r, REAL_EXCHANGE, p::EXCHANGE_LEN, false, last);
  check(t, done == 2, "receiver: two exchanges back to back");
}

inline int run_all() {
  Tally t;
  test_crc(&t);
  test_rejects(&t);
  test_decode_real(&t);
  test_limits(&t);
  test_helpers(&t);
  test_receiver(&t);
  printf("RESULT pass=%d fail=%d\n", t.pass, t.fail);
  fflush(stdout);
  return t.fail;
}

}  // namespace heater_protocol_test

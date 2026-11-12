# Glue tests

`glue_test.cpp` builds the real `components/diesel_heater/heater_uart.cpp`
natively, against a small fake of the ESPHome API in `fake/`. No device.
No ESPHome install. A manual clock and a fake UART drive the component.

```sh
tests/glue/run.sh
```

Needs `g++` with C++17. The build uses `-Wall -Wextra -Werror`,
AddressSanitizer and UndefinedBehaviorSanitizer. It runs the protocol
checks of `tests/host/protocol_test.h` first, then the glue checks. It
prints one `PASS` or `FAIL` line per check, and one
`RESULT pass=N fail=M` line for each part. It exits 1 on any failure.
Extra compiler flags pass through: `run.sh -DFOO`.

What it covers:

- Receive: a real exchange, every reject reason, an exchange split over
  two `loop()` calls, an idle gap inside an exchange, one exchange per
  `loop()` and the 128-byte budget,
  a failed `read_byte()`.
- Watchdog: link up at exactly 10 s, down after it, measurements stop and
  resume, a `millis()` wrap.
- Publish: discrete values on change and on every `update()`, the
  transient state `0x10`, range rejects, both error bytes, the hex rate
  limit, no optional sensor wired.
- Serial commands: the frame copied from the controller, the ON and OFF
  bytes, the CRC, the 24-byte echo limit, a stray byte before the
  exchange, no valid exchange, a skip while bytes wait, a skip after a
  loop stall over 30 ms, expiry after 3 s.
- Duty control: pulses and their timing, a corrective round, giving up
  after 5 rounds, clamping, waiting for two equal samples.
- A seeded random byte stream with random gaps, commands and duty
  targets, 20000 rounds, under the sanitizers.

## The fake

`fake/esphome/` holds only what `heater_uart.cpp` and `heater_uart.h`
include. Sensors record every published value. Switches record every
edge with its time. `delay()` advances the fake clock. The UART can echo
TX bytes and append a reply after them, as the shared bus pin does.

A change to the ESPHome API that the component uses needs the same
change here. The ESPHome build (`esphome compile`) stays the check
against the real API.

## Known behaviour

`read_bus_()` reads at most 128 bytes per call, and stops at the end of
each valid exchange. The rest is read on the next `loop()`.

A serial command goes out only right after a valid exchange, and only
when that exchange is recent: no byte waits behind it, and `read_bus_()`
started at most 30 ms after the previous call. The UART gives no arrival
time, so these two checks stand in for it. A skipped command waits for
the next exchange, until it expires after 3 s.

## AddressSanitizer startup crash

GCC 12 AddressSanitizer can crash before `main()` on a kernel with high
mmap randomization. The log then holds no test line. `run.sh` retries
that case only, up to 5 times. Any crash after the first test line is a
real failure.

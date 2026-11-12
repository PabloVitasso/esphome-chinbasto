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
  two `loop()` calls, an idle gap inside an exchange, the 128-byte budget,
  a failed `read_byte()`.
- Watchdog: link up at exactly 10 s, down after it, measurements stop and
  resume, a `millis()` wrap.
- Publish: discrete values on change and on every `update()`, the
  transient state `0x10`, range rejects, both error bytes, the hex rate
  limit, no optional sensor wired.
- Serial commands: the frame copied from the controller, the ON and OFF
  bytes, the CRC, the 24-byte echo limit, the receiver reset, no frame
  yet, expiry after 3 s.
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

`read_bus_()` stops at the 48th byte after an idle gap, also with no
command pending. That byte is the command slot. The rest of the bytes
are read on the next `loop()`.

## AddressSanitizer startup crash

GCC 12 AddressSanitizer can crash before `main()` on a kernel with high
mmap randomization. The log then holds no test line. `run.sh` retries
that case only, up to 5 times. Any crash after the first test line is a
real failure.

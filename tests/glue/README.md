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

What it covers, in priority order. Most of the budget goes to the first
two groups:

1. Realistic scenarios, one real situation each: a cold start through
   pre-heat and ignition to a steady run; an E-05 trip with a 250 °C
   peak between two updates; the restart that clears the held error; a
   40 s link loss mid-run; a panel Hz change; a debug session; a night
   with the heater off, 8 h, as a message budget.
2. Valid boundaries: every field at its limit (chamber −50 and 400 °C,
   fan 8000 RPM, pump 6.0 Hz, demand 8 and 35, state 8, error 13), and
   the policy timing at exactly 60 s and 5 min, across a `millis()` wrap,
   at the sample cap.
3. Invalid boundaries: one step past each limit. Never published,
   always counted.
4. Structure: framing, CRC, idle gaps, the byte budget, a failed read,
   the serial command slot, the 50 ms wait without blocking, a lost slot
  when the bus speaks, the echo limit and window, duty control.
5. Randomized fuzzing: one seeded stream of 20000 rounds, under the
   sanitizers.

The tests read only what a consumer sees: the sensors, the texts, the
debug JSON, the bytes on the bus.

## The fake

`fake/esphome/` holds only what `heater_uart.cpp` and `heater_uart.h`
include. Sensors record every published value. Switches record every
edge with its time. `delay()` advances the fake clock. The UART can echo
TX bytes and append a reply after them, as the shared bus pin does.

A change to the ESPHome API that the component uses needs the same
change here. The ESPHome build (`esphome compile`) stays the check
against the real API.

## Known behaviour

The publish policy lives in `components/diesel_heater/publish_policy.h`:

- Discrete values (state, on/off, errors, demand, link): a change goes
  out at once, from the exchange path. Everything again every 5 min.
  After a link loss, everything again at once.
- Measurements: one 60 s window. Pump last value, fan mean, chamber last
  value and maximum. The first window goes out as soon as the link is
  up.
- Debug data: one JSON text per update, only while `set_debug(true)`.

`read_bus_()` reads at most 128 bytes per call, and stops at the end of
each valid exchange.

A serial command is armed right after a valid exchange, unless the loop
stalled over 30 ms before it read the exchange. It goes out from
`loop()` 50 ms later, only if no byte arrived in between; otherwise the
slot is lost and it waits for the next exchange, until it expires after
3 s. Nothing blocks: no `delay()`, no `flush()`. The echo of our own 24
bytes is dropped in the read path, inside a 40 ms window.

## AddressSanitizer startup crash

GCC 12 AddressSanitizer can crash before `main()` on a kernel with high
mmap randomization. The log then holds no test line. `run.sh` retries
that case only, up to 5 times. Any crash after the first test line is a
real failure.

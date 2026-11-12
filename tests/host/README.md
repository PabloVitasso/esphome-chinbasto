# Host tests

`protocol-test.yaml` builds `heater_protocol.h` natively, on the ESPHome
`host` platform, and runs the checks in `protocol_test.h`. No device.

```sh
esphome run tests/host/protocol-test.yaml
```

The run prints one `PASS` or `FAIL` line per check, then
`RESULT pass=N fail=M`. It exits 1 on any failure.

The fixture `REAL_EXCHANGE` is a real 48-byte exchange, captured from a
heater with the heater off. Both CRCs are valid.

# TODO

Open work for this fork. The larger plans are in the README,
[Roadmap](README.md#roadmap).

## Rename the two error entities

**Why:** "Active Error (Code)" reads 1 while the heater runs with no
error. The name suggests a fault. The value is the heater's live error
state, exchange byte 27:

| Value | Meaning |
|---|---|
| 0 | Off |
| 1 | Running, no error |
| *N*+1 | Error E-0*N*, for example 6 = E-05 Too hot |

"Error (Code)" is exchange byte 41: the last error, held until the next
start.

**Do:**

1. Rename "Active Error (Code)" to "Error State (live)".
2. Add a text entity next to it, from the same byte. Examples:
   "Off", "Running, no error", "E-05 Too hot".
3. Rename "Error (Code)" to "Last Error (code)".
4. Keep the MQTT topics: `active_error_code`, `error_code`,
   `error_description`. Consumers do not change.
5. Update the example yaml and the README key table.
6. Glue tests for the new text entity: every value 0 to 14, and one
   value out of range.

**Needs:** an OTA on the device. Entity names change in the web
interface and in Home Assistant, if used.

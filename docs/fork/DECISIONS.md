# Design decisions

Each entry records the problem, the options considered, the choice and the
reason. Code changes carry a `FORK:` comment pointing to the entry number.
Byte offsets refer to the reassembled payload (16-byte frame headers removed)
unless stated otherwise; see [PROTOCOL-inYT-v65.md](PROTOCOL-inYT-v65.md).

## 1. Read the status block inside combined config+status messages

**Problem.** After an ESP32 restart, temperatures and heater state stayed
unknown until the spa sent a standalone status message, which it only does
when something changes (a pump, the light, the heater). On an idle spa that
can take a long time. The spa sends a combined config+status message after
every GO handshake (every 23 s), but upstream decoded the embedded status only
for a 390-byte layout. inYT version 65 sends 353 bytes: 208 bytes of config
followed by the full 145-byte status block. Upstream's generic fallback did
not match either, because it expects the byte before the status block to be
`0x3B` (it is `0xDE` here).

**Options.** (a) Add a second hard-coded length. (b) A table of known layouts
used by one code path. (c) Search the message for the status marker.

**Choice.** (b): `protocol::COMBINED_LAYOUTS` holds {390, 162} and {353, 145}.
The existing checks are kept unchanged for both: marker `01 00`, set point
8 to 40 °C, water temperature 0 to 45 °C.

**Rejected.** Requiring the status set point to equal the config set point.
While an economy program is active the status set point drops (by 20 °F on
the tested pack) and the config value does not, so valid messages would be
rejected. Option (c) risks matching inside config data.

**Effect.** Temperatures and outputs are known within one handshake (at most
23 s) of a restart. Verified by replay (`restart_temperatures_from_first_combined_message`)
and by the host end-to-end test.

## 2. Save the program reported by the spa

**Problem.** The spa reports its program in an 18-byte message only when it
starts up and when the program changes. Upstream saved the program to flash
only when it was changed from Home Assistant, so after an ESP32 restart the
select showed the last value set from Home Assistant (or nothing), even if the
program had since been changed at the keypad.

**Choice.** `GeckoSpaSelect::set_reported_program()` publishes the reported
program and saves it when it differs from the saved value, or when nothing
has been saved yet. `setup()` already restores the saved value. The existing
preference key (the select's object ID hash) is kept, so values saved by
upstream builds are still restored after updating.

**Flash wear.** One write per actual program change. Reporting the same
program again does not write (`same_program_reported_again_does_not_write_flash`).

**Edge case handled.** With nothing saved, the in-memory default index is
Standard. A first report of Standard is still saved, otherwise it could not
be restored (`first_report_of_default_program_is_saved`).

## 3. Read-only heater, circulation and pump sensors

**Problem.** The heater state was only visible as the climate action, and
pump states only through switches that also send commands.

**Choice.** New `binary_sensor` types `heater` (status byte 6 bit 5),
`circulation` (byte 6 bit 2), `pump1` and `pump2` (byte 7, bits 0-1 and 2-3).
A pump sensor is on for any non-zero state. They are published from the same
decoded values as the existing entities and send nothing to the spa.

**Not added.** A pump speed sensor: the tested pumps are single speed and
always report the value geckolib calls HIGH. A speed sensor can be added once
it can be tested with a two-speed pump. Sensors for pumps 3 and 4 are left out
for the same reason (decoded identically, but untested).

## 4. Option `report_cool_mode` on the climate entity

**Problem.** When the water is warmer than the set point the climate entity
reports mode `cool` and action `cooling`. The spa cannot cool. This happens
routinely while an economy program lowers the set point.

**Choice.** Climate option `report_cool_mode`, default `true` (upstream
behaviour). With `false`, the mode is always `heat`, the only supported mode
is `heat`, and the action is `heating` when the heater bit is set and `idle`
otherwise.

## 5. Option `log_status_changes`

**Problem.** At DEBUG level every status message produces raw proxy lines, a
hex dump and four decoded INFO lines, many times a minute, which hides the
lines that matter.

**Choice.** Component option `log_status_changes`, default `false` (upstream
behaviour, output unchanged). With `true`: raw proxy lines and hex dumps move
to VERBOSE; decoded status lines are logged only when the status block
differs from the previous one, preceded by one line listing the changed
bytes, for example `Status change (3 bytes): [51]00>78 [53]00>01 [61]D8>A1`.
Entity updates do not depend on this option.

**Note.** Recording replay fixtures needs the raw proxy lines: use
`log_status_changes: false`, or set the logger to VERBOSE for `gecko_spa`.

## 6. Set point sensor from the config message

**Problem.** The climate target shows the effective set point from the status
block, which drops during economy periods. The set point the user chose was
not visible at those times.

**Choice.** `sensor` type `set_point`, published from config bytes 3-4 when
the value is plausible (8 to 40 °C) and has changed. The climate entity is
unchanged.

## 7. Keep spa-initiated frames out of config/status reassembly

**Problem.** Frames carry a channel in header byte 6: `0x0A` for config and
status replies, `0x0B` for messages the spa sends on its own (program,
reminders, program schedule data). Upstream appended every multi-part frame
to one buffer. After a program change, and at power-up, the spa sends a burst
of program schedule frames (type `0x4E`, 16 or more parts) that has no final
part and is interleaved with config/status parts. The shared buffer overflowed
or produced messages of the wrong length, and the config/status message in
progress was lost.

**Options.** (a) Separate buffers per channel. (b) Ignore multi-part frames on
channel `0x0B`. (c) Timeout for partial messages.

**Choice.** (b), in `protocol::is_spa_initiated_part()`. The 18-byte program
and 77-byte reminder messages, also on channel `0x0B`, are handled before this
point and are unaffected. (a) would only move the problem: the schedule burst
never completes, so its buffer would fill and later mix with the next burst.
(c) risks discarding valid messages when the ESP32 main loop stalls.

**Basis.** Upstream's protocol notes show the same header convention
(`17 09 00 00 00 17 0A 01 ...` for status parts, channel `0x0B` for program
and reminder messages). On the tested pack every config/status part uses
`0x0A`. Verified with recorded traffic in which a schedule burst is
interleaved with a 353-byte message (`program_schedule_burst_does_not_corrupt_reassembly`).

## 8. ESP32-S3-Zero example and hardware notes

An example configuration for the Waveshare ESP32-S3-Zero
(`esphome/spa-controller-s3-zero.yaml`) and wiring notes
([HARDWARE-S3-ZERO.md](HARDWARE-S3-ZERO.md)). The S3-Zero has no USB-serial
chip, so the logger uses `hardware_uart: USB_SERIAL_JTAG`.

## 9. No bare `min()` in `loop()`

**Problem.** The reset back-off used `min(...)`, which exists only because the
Arduino framework defines it. The component did not compile for the ESP-IDF
framework or ESPHome's host platform.

**Choice.** An explicit conditional expression with the same result. This
also makes the host build used by the end-to-end test possible.

## 10. Testing approach

The component is tested in two ways, both from recorded bus traffic:

- **Replay tests** (`tests/test_gecko_spa.cpp`): the real component source is
  compiled with g++ against small stand-ins for the ESPHome classes it uses
  (`tests/stubs`), and fixture frames are fed through its UART input.
  Expected values were decoded from the fixtures independently. Disabling
  any of changes 1 to 7 makes at least one replay test fail; without change
  9 the host build does not compile.
- **Host end-to-end test** (`tests/e2e`): the full firmware is built for
  ESPHome's host platform with the real ESPHome sources, a pseudo-terminal
  takes the place of the Arduino proxy, and entity states are read back over
  the native API. This covers the YAML schema, code generation, entity
  registration and preference storage across a restart.

Fixtures are sanitised with `tools/sanitise_capture.py`: the clock message and
the status "Hours" byte are set to fixed values and the reminder message is
dropped, so fixtures carry no record of when they were captured.

## Not changed

Pump and circulation commands, the Arduino firmware, the reminder message
handling and the climate entity's use of the effective set point are as
upstream.

# Changelog

Changes in this fork relative to [LDWTH/esphome-gecko](https://github.com/LDWTH/esphome-gecko).
Numbers refer to [docs/fork/DECISIONS.md](docs/fork/DECISIONS.md).

## fork-v1.0.0

Based on upstream commit `614d2c0`. The on-spa acceptance checklist in
[docs/fork/TESTING.md](docs/fork/TESTING.md) passed on an inYT pack
(config/status version 65) with an ESP32-S3-Zero, including 24 hours without
unexpected warnings and the heater sensor matching the pack's power draw
through three heat cycles.

### Fixed

- Temperatures and outputs were unknown after an ESP32 restart until something
  changed on the spa. The status block in the 353-byte combined message is now
  decoded (#1). Verified by replay and host end-to-end tests.
- The program changed at the keypad was lost after an ESP32 restart. It is now
  saved when reported (#2). Verified by replay tests and an end-to-end restart
  with preferences kept.
- After a program change or at power-up, the spa's program schedule burst was
  appended to the config/status buffer, overflowing it and losing the message
  in progress. Spa-initiated multi-part frames are now ignored (#7). Verified
  with a recording in which the burst is interleaved with a combined message.
- The component did not compile without the Arduino `min()` macro (#9).
  Verified by building for ESPHome's host platform.

### Added

- `binary_sensor` types `heater`, `circulation`, `pump1`, `pump2` (#3).
- `sensor` type `set_point` (#6).
- Climate option `report_cool_mode` (#4).
- Component option `log_status_changes` (#5).
- ESP32-S3-Zero example configuration and hardware notes (#8).
- Replay tests, host end-to-end test, capture sanitiser, privacy check, CI.

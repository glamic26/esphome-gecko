# About this fork

This fork of [LDWTH/esphome-gecko](https://github.com/LDWTH/esphome-gecko)
(itself a fork of [zteifel/esphome-gecko](https://github.com/zteifel/esphome-gecko))
adds fixes and read-only sensors for the inYT pack with config/status
version 65, plus tests built from recorded bus traffic. Existing
configurations keep working unchanged: every new behaviour is either a fix or
an option whose default is upstream's behaviour.

Changes in this fork were developed with AI assistance (Claude Code) and
tested on real hardware. Review accordingly.

Status: pre-release. Tested by replaying traffic recorded on real hardware;
on-spa acceptance of this build is pending.

## Changes

| # | Change | Default |
|---|---|---|
| 1 | Status read from the 353-byte combined config+status message, so temperatures are known within 23 s of a restart | always on |
| 2 | Program reported by the spa is saved to flash and restored after a restart | always on |
| 3 | Read-only `binary_sensor` types `heater`, `circulation`, `pump1`, `pump2` | opt-in |
| 4 | Climate option `report_cool_mode` | `true` (upstream) |
| 5 | Component option `log_status_changes` | `false` (upstream) |
| 6 | `sensor` type `set_point`: the user's set point, unaffected by economy periods | opt-in |
| 7 | Spa-initiated frames kept out of config/status reassembly | always on |
| 8 | ESP32-S3-Zero example configuration and hardware notes | |
| 9 | Builds without the Arduino `min()` macro (ESP-IDF and host platform) | always on |

Reasons and alternatives for each: [DECISIONS.md](DECISIONS.md). Protocol
details: [PROTOCOL-inYT-v65.md](PROTOCOL-inYT-v65.md). Tests:
[TESTING.md](TESTING.md). Release notes: [CHANGELOG.md](../../CHANGELOG.md).

Tested with: inYT pack, `inYT_C65.xml` / `inYT_S65.xml`, in.k500 keypad, two
single-speed pumps, ESP32-S3-Zero with an ATmega328P Nano, ESPHome 2026.10.2.

## Configuration

```yaml
gecko_spa:
  id: spa
  uart_id: arduino_uart
  reset_pin: GPIO7
  log_status_changes: true        # optional, default false

climate:
  - platform: gecko_spa
    gecko_spa_id: spa
    name: "Spa"
    report_cool_mode: false       # optional, default true

binary_sensor:
  - platform: gecko_spa
    gecko_spa_id: spa
    type: heater                  # also: circulation, pump1, pump2
    name: "Spa Heater"
    device_class: heat

sensor:
  - platform: gecko_spa
    gecko_spa_id: spa
    type: set_point
    name: "Spa Set Point"
```

A complete example: [`esphome/spa-controller-s3-zero.yaml`](../../esphome/spa-controller-s3-zero.yaml).

## Using a release

Point ESPHome at a tag, so the device only changes when you change the tag:

```yaml
external_components:
  - source: github://glamic26/esphome-gecko@fork-v1.0.0
    components: [gecko_spa]
```

To roll back, set an earlier tag (or upstream's repository) and install again.
Values saved in flash (program, reminders, climate target) use the same keys
as upstream and are kept either way.

## Branches

- `master` mirrors upstream's `master` and is never edited here.
- `spa-fixes` holds this fork's changes and is the default branch.
- Tags `fork-vX.Y.Z` mark tested releases. Upstream changes are merged, not
  rebased, so tags never move.

## Licence

Same licence as upstream (MIT, as stated in the upstream README).

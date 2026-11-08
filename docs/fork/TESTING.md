# Testing

## Replay tests

```sh
tests/run.sh        # add -v to see the component's log output
```

Needs only g++ (C++17). The real component source is compiled against the
stand-ins in `tests/stubs` and fed the recorded frames in `tests/fixtures`.

| Fixture | Contents |
|---|---|
| `restart_steady.rx` | Bridge restart while the spa runs; pump 1, pump 2 and the light switched at the keypad |
| `power_up.rx` | Spa powered up while the bridge runs: start-up burst, program report, water temperature 0 until valid, heater on |
| `program_changes.rx` | Program changed at the keypad: Super Energy, Weekend, Away, Standard |

## Host end-to-end test

```sh
pip install esphome==2026.10.2
python3 tests/e2e/run_host_e2e.py
```

Linux only. Builds `tests/e2e/host-e2e.yaml` for ESPHome's host platform,
runs it with a pseudo-terminal in place of the Arduino, plays fixtures into it
and checks the entities over the native API, including a restart with
preferences kept.

## Recording a fixture

1. Set `log_status_changes: false` (or the logger to VERBOSE for `gecko_spa`)
   and the logger level to DEBUG, so the raw `Proxy: RX:` lines are logged.
2. Save the log in chronological order.
3. Convert it:

   ```sh
   tools/sanitise_capture.py capture.log tests/fixtures/name.rx --note "What happens in it"
   ```

   This keeps only the raw frames, sets the clock and status "Hours" byte to
   fixed values, drops reminder messages and recomputes checksums.
4. Run `tools/privacy_check.py` before committing.

## Privacy check

```sh
tools/privacy_check.py                     # all tracked files
tools/privacy_check.py --commits A..B      # also commit authors and messages
```

Set `PRIVACY_TERMS_FILE` to a file outside the repository with extra terms to
search for, one per line.

## On-spa acceptance

Before tagging a release, on real hardware:

- [ ] After the spa is powered on, the bridge connects and the keypad shows no error.
- [ ] After an ESP32 restart, water temperature and set point appear within 23 s.
- [ ] After an ESP32 restart, the program shows the last program set at the keypad.
- [ ] The heater sensor matches the pack's power draw through one heat cycle.
- [ ] Each pump sensor turns on and off with the keypad presses.
- [ ] The set point can be changed from Home Assistant and from the keypad, and both show the new value.
- [ ] Restarting the Nano (button) and the ESP32 both recover without intervention.
- [ ] 24 hours without unexpected warnings in the log.

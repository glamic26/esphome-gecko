#!/usr/bin/env python3
"""End-to-end test of the real firmware on ESPHome's host platform.

Builds tests/e2e/host-e2e.yaml with `esphome compile`, runs the program with a
pseudo-terminal in place of the Arduino proxy, plays recorded proxy lines into
it and reads the entity states back over the native API, as Home Assistant
would. Linux only. Needs `esphome` and `aioesphomeapi` (installed with ESPHome).

Usage: run_host_e2e.py [--skip-build]
"""

import argparse
import asyncio
import math
import os
import pty
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import tty
from pathlib import Path

from aioesphomeapi import APIClient, ClimateAction, ClimateMode

HERE = Path(__file__).resolve().parent
FIXTURES = HERE.parent / "fixtures"
CONFIG = HERE / "host-e2e.yaml"
TTY_LINK = "/tmp/gecko-e2e-tty"
API_PORT = 16053

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def frames(name, start=0, stop=None):
    lines = [l.strip() for l in (FIXTURES / name).read_text().splitlines() if l.startswith("RX:")]
    return lines[start:stop]


class Firmware:
    """The host program, attached to one end of a pseudo-terminal."""

    def __init__(self, binary, prefdir):
        self.binary = binary
        self.prefdir = prefdir
        self.proc = None
        self.master = None

    def start(self):
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        if os.path.lexists(TTY_LINK):
            os.unlink(TTY_LINK)
        os.symlink(os.ttyname(slave), TTY_LINK)
        os.set_blocking(self.master, False)
        env = dict(os.environ, ESPHOME_PREFDIR=self.prefdir)
        self.log = open(Path(self.prefdir) / "firmware.log", "ab")
        self.proc = subprocess.Popen([self.binary], env=env, stdout=self.log, stderr=subprocess.STDOUT)
        self._slave = slave

    def drain(self):
        try:
            while os.read(self.master, 4096):
                pass
        except (BlockingIOError, OSError):
            pass

    async def feed(self, lines, gap=0.02):
        for line in lines:
            os.write(self.master, (line + "\n").encode())
            await asyncio.sleep(gap)
            self.drain()

    def stop(self):
        if self.proc:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        for fd in (self.master, self._slave):
            try:
                os.close(fd)
            except OSError:
                pass
        self.log.close()


class Api:
    def __init__(self):
        self.client = APIClient("127.0.0.1", API_PORT, None)
        self.by_key = {}
        self.states = {}

    async def connect(self, timeout=20):
        deadline = time.monotonic() + timeout
        while True:
            try:
                await self.client.connect(login=True)
                break
            except Exception:  # firmware still starting
                if time.monotonic() > deadline:
                    raise
                await asyncio.sleep(0.3)
        entities, _ = await self.client.list_entities_services()
        self.by_key = {e.key: e.object_id for e in entities}
        self.client.subscribe_states(self._on_state)
        await asyncio.sleep(0.5)

    def _on_state(self, s):
        self.states[self.by_key.get(s.key, s.key)] = s

    def get(self, object_id):
        return self.states.get(object_id)

    async def close(self):
        await self.client.disconnect()


def near(a, b, tol=0.05):
    return a is not None and not math.isnan(a) and abs(a - b) <= tol


async def scenario(binary, prefdir):
    # 1. Fresh bridge, spa running: temperatures arrive from the first
    #    combined message, before any standalone status (fixture frame 28).
    fw = Firmware(binary, prefdir)
    fw.start()
    api = Api()
    await api.connect()
    print("restart while the spa runs")
    await fw.feed(frames("restart_steady.rx", 0, 10))
    await asyncio.sleep(0.3)
    c = api.get("spa")
    check(c is not None and near(c.current_temperature, 37.5), "climate current 37.5 from combined message")
    check(c is not None and near(c.target_temperature, 37.5), "climate target 37.5 from combined message")
    check(api.get("spa_set_point") is not None and near(api.get("spa_set_point").state, 37.5), "set point sensor 37.5")
    check(api.get("spa_connected") is not None and api.get("spa_connected").state, "connected")

    # 2. Program changes at the panel; stop after Weekend is reported.
    print("program change")
    await fw.feed(frames("program_changes.rx", 0, 43))
    await asyncio.sleep(0.3)
    p = api.get("spa_program")
    check(p is not None and p.state == "Weekend", "program select shows Weekend")
    c = api.get("spa")
    check(c is not None and c.mode == ClimateMode.HEAT, "climate mode stays heat (report_cool_mode: false)")
    await asyncio.sleep(1.5)  # preferences are written every 1 s in this build
    await api.close()
    fw.stop()

    # 3. ESP restart: the program is restored from flash before the spa
    #    says anything.
    print("ESP restart")
    fw = Firmware(binary, prefdir)
    fw.start()
    api = Api()
    await api.connect()
    p = api.get("spa_program")
    check(p is not None and p.state == "Weekend", "program restored as Weekend after restart")

    # 4. Spa power-up: zero temperature is not shown, heater comes on.
    print("spa power-up")
    zero_seen = []
    orig = api._on_state

    def watch(s):
        orig(s)
        if api.by_key.get(s.key) == "spa" and not math.isnan(s.current_temperature) and s.current_temperature < 1:
            zero_seen.append(s.current_temperature)

    api.client.subscribe_states(watch)
    await fw.feed(frames("power_up.rx", 8))
    await asyncio.sleep(0.3)
    c = api.get("spa")
    check(not zero_seen, "no 0.0 water temperature published")
    check(c is not None and near(c.current_temperature, 37.0), "climate current 37.0 once valid")
    check(c is not None and c.action == ClimateAction.HEATING, "climate action heating")
    check(api.get("spa_heater") is not None and api.get("spa_heater").state, "heater sensor on")
    check(api.get("spa_circulation_pump") is not None and api.get("spa_circulation_pump").state, "circulation sensor on")
    check(api.get("spa_program") is not None and api.get("spa_program").state == "Standard", "program Standard after power-up")
    check(api.get("spa_status_version") is not None and api.get("spa_status_version").state == "inYT_S65.xml", "status version")
    await api.close()
    fw.stop()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-build", action="store_true")
    args = ap.parse_args()

    esphome = shutil.which("esphome") or str(Path(sys.executable).with_name("esphome"))
    if not args.skip_build:
        subprocess.run([esphome, "compile", str(CONFIG)], check=True, cwd=HERE)
    binary = HERE / ".esphome" / "build" / "gecko-e2e" / ".pioenvs" / "gecko-e2e" / "program"
    if not binary.exists():
        sys.exit(f"firmware not found: {binary}")

    with tempfile.TemporaryDirectory() as prefdir:
        try:
            asyncio.run(scenario(str(binary), prefdir))
        finally:
            log = Path(prefdir) / "firmware.log"
            if failures and log.exists():
                print("---- firmware log (last 60 lines) ----")
                print("\n".join(log.read_text(errors="replace").splitlines()[-60:]))
    if os.path.lexists(TTY_LINK):
        os.unlink(TTY_LINK)
    print(f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()

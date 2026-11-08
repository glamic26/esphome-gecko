#!/usr/bin/env python3
"""Fails if tracked files contain data that should not be published.

Checks every file tracked by git (or the paths given) for:
  * IPv4 addresses (other than loopback and 0.0.0.0) and MAC addresses
  * email addresses (GitHub noreply addresses are allowed)
  * literal API encryption keys and passwords in YAML (placeholders and
    !secret references are allowed)
  * dates or times in test fixtures, and fixture clock / status "Hours" bytes
    that are not the neutral values written by tools/sanitise_capture.py

Optionally, terms listed one per line in the file named by the environment
variable PRIVACY_TERMS_FILE are also searched for (case-insensitive). Keep that
file outside the repository.

A line can be exempted by adding the marker `privacy-ok` to it.

Usage: privacy_check.py [--commits RANGE] [PATH ...]
"""

import argparse
import os
import re
import subprocess
import sys

IPV4 = re.compile(r"(?<![\d.])((25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(25[0-5]|2[0-4]\d|1?\d?\d)(?![\d.])")
MAC = re.compile(r"\b[0-9A-Fa-f]{2}([:-][0-9A-Fa-f]{2}){5}\b")
EMAIL = re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
ALLOWED_EMAIL = re.compile(r"@users\.noreply\.github\.com$|^noreply@anthropic\.com$")
YAML_KEY = re.compile(r"\bkey:\s*[\"']?[A-Za-z0-9+/]{43}=")
YAML_PASSWORD = re.compile(r"password:\s*[\"']([^\"']+)[\"']", re.I)
PLACEHOLDER = re.compile(r"^(your|replace|change|example|xxx)", re.I)
TIMESTAMP = re.compile(r"\d{4}-\d\d-\d\d|\b\d\d:\d\d:\d\d\b")

NEUTRAL_HOUR = 0x0C
STATUS_OFFSETS = {145: 0, 162: 0, 353: 208, 390: 228}


def tracked_files():
    out = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout
    return [f for f in out.splitlines() if f]


def load_terms():
    path = os.environ.get("PRIVACY_TERMS_FILE")
    if not path:
        return []
    with open(path, encoding="utf-8") as fh:
        return [t.strip().lower() for t in fh if t.strip() and not t.startswith("#")]


def check_text(path, text, terms, problems):
    for n, line in enumerate(text.splitlines(), 1):
        if "privacy-ok" in line:
            continue
        where = f"{path}:{n}"
        for m in IPV4.finditer(line):
            if m.group(0) not in ("127.0.0.1", "0.0.0.0"):
                problems.append(f"{where}: IPv4 address")
        if MAC.search(line):
            problems.append(f"{where}: MAC address")
        for m in EMAIL.finditer(line):
            if not ALLOWED_EMAIL.search(m.group(0)):
                problems.append(f"{where}: email address")
        if path.endswith((".yaml", ".yml")) or "```" in text:
            if YAML_KEY.search(line):
                problems.append(f"{where}: literal encryption key")
            m = YAML_PASSWORD.search(line)
            if m and not PLACEHOLDER.match(m.group(1)):
                problems.append(f"{where}: literal password")
        low = line.lower()
        for t in terms:
            if t in low:
                problems.append(f"{where}: private term")


def check_fixture(path, text, problems):
    frames = []
    for n, line in enumerate(text.splitlines(), 1):
        if TIMESTAMP.search(line):
            problems.append(f"{path}:{n}: date or time in fixture")
        if line.startswith("RX:"):
            frames.append(bytes.fromhex(line.split(":")[2]))
    buf = b""
    for f in frames:
        x = 0
        for b in f[:-1]:
            x ^= b
        if x != f[-1]:
            problems.append(f"{path}: frame with bad checksum {f[:16].hex()}")
        if len(f) == 77 and f[6] == 0x0B:
            problems.append(f"{path}: reminder message present (carries dates)")
        if len(f) == 22 and f[13] == 0x4B and tuple(f[18:21]) != (NEUTRAL_HOUR, 0, 0):
            problems.append(f"{path}: clock message not neutralised")
        single = len(f) in (15, 18, 33, 77) or (len(f) == 22 and f[13] == 0x4B)
        if single or len(f) < 16 or f[1] != 0x09 or f[6] == 0x0B:
            continue
        buf += f[16:]
        if f[9] == 0x01:
            continue
        start = STATUS_OFFSETS.get(len(buf))
        if start is not None and buf[start + 2] != NEUTRAL_HOUR:
            problems.append(f"{path}: status Hours byte not neutralised")
        buf = b""


def check_commits(rng, terms, problems):
    log = subprocess.run(
        ["git", "log", "--format=%H%n%an <%ae>%n%cn <%ce>%n%B%n--end--", rng],
        capture_output=True, text=True, check=True,
    ).stdout
    for block in log.split("--end--"):
        lines = block.strip().splitlines()
        if not lines:
            continue
        sha = lines[0][:10]
        check_text(f"commit {sha}", "\n".join(lines[1:]), terms, problems)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--commits", help="also check authors and messages of this git range")
    args = ap.parse_args()

    terms = load_terms()
    problems = []
    for path in args.paths or tracked_files():
        try:
            with open(path, encoding="utf-8") as fh:
                text = fh.read()
        except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
            continue  # binary files (images, firmware) and removed paths
        check_text(path, text, terms, problems)
        if path.startswith("tests/fixtures/") and path.endswith(".rx"):
            check_fixture(path, text, problems)
    if args.commits:
        check_commits(args.commits, terms, problems)

    for p in problems:
        print(p)
    if problems:
        print(f"{len(problems)} problem(s) found", file=sys.stderr)
        return 1
    print("privacy check passed" + (f" ({len(terms)} private terms)" if terms else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())

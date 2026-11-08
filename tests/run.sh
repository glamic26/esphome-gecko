#!/bin/sh
# Builds the component against the host stubs and runs the replay tests.
# Usage: tests/run.sh [-v]   (-v echoes component log output)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out="${TMPDIR:-/tmp}/gecko_spa_tests"
${CXX:-g++} -std=gnu++17 -O1 -g -Wall -Wextra -Wno-format-truncation \
  -I "$here/stubs" -I "$here" -DFIXTURE_DIR="\"$here/fixtures\"" \
  "$here/test_gecko_spa.cpp" -o "$out"
"$out" "$@"

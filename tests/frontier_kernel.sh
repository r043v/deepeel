#!/bin/sh
# Kernel frontier CLI contract: a walled run saves frontier.bin, `frontier stats` reads it,
# `frontier drain` resumes it (the frontier keeps shrinking or growing with new forks, never lost).
# usage: frontier_kernel.sh <deepeel> <rom.gb>
set -e
D=$1; ROM=$2; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
"$D" explore "$ROM" --core --state-dir "$W" --max-total 20000 >/dev/null 2>&1
test -s "$W/frontier.bin"
"$D" frontier stats "$W" | grep -Eq "^frontier: [1-9][0-9]* kernel states pending"
"$D" frontier drain "$ROM" "$W" >/dev/null 2>&1
"$D" frontier stats "$W" | grep -q "^frontier:"
test -s "$W/coverage.bin"

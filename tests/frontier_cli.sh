#!/bin/sh
# CLI contract of the frontier: a spooled run converts to lots, `frontier stats`
# reads them, `frontier drain` empties them. usage: frontier_cli.sh <deepeel> <rom>
set -e
D=$1; ROM=$2; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
"$D" explore "$ROM" --state-dir "$W" --queue 1 --queue-max 1 --max-paths 5 >/dev/null 2>&1
test -s "$W/spool.bin"
"$D" explore "$ROM" --state-dir "$W" --resume --spool-to-frontier 2>&1 | grep -q "spool records converted"
"$D" frontier stats "$W" | grep -q "^frontier:"
"$D" frontier drain "$ROM" "$W" --tier inf >/dev/null 2>&1
"$D" frontier stats "$W" | grep -q "^frontier:"
# portable lots (Sega): a walled run leaves a lot, `frontier stats` reads it, `frontier drain` empties it
if [ -n "$2" ] && [ -n "$SEGA_ROM" ]; then
  L="$W/sega.dpl"
  "$D" z80 explore sms "$SEGA_ROM" --lot "$L" --max-total 5 >/dev/null 2>&1
  "$D" frontier stats "$L" | grep -q "1 records pending"
  "$D" frontier drain "$SEGA_ROM" "$L" --sys sms >/dev/null 2>&1
  "$D" frontier stats "$L" | grep -q "^frontier: empty"
fi

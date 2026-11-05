#!/bin/sh
# --resume must not forget what the previous pass proved: the coverage map is saved in
# <state-dir>/coverage.bin and OR-ed back, so the resumed report never knows less.
# usage: resume_cov.sh <deepeel> <rom.gb>
set -e
D=$1; ROM=$2; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
"$D" explore "$ROM" --unbounded --state-dir "$W/st" --summary "$W/a.json" >/dev/null 2>&1
test -s "$W/st/coverage.bin" || { echo "coverage.bin not written"; exit 1; }
"$D" explore "$ROM" --unbounded --state-dir "$W/st" --resume --summary "$W/b.json" >"$W/b.out" 2>"$W/b.err"
grep -q "resumed coverage map" "$W/b.err" || { echo "coverage map not resumed"; cat "$W/b.err"; exit 1; }
KA=$(grep -o '"known_pct":[0-9.]*' "$W/a.json" | head -1 | cut -d: -f2)
KB=$(grep -o '"known_pct":[0-9.]*' "$W/b.json" | head -1 | cut -d: -f2)
awk -v a="$KA" -v b="$KB" 'BEGIN { exit !(b + 0 >= a + 0) }' || { echo "resumed run knows less: $KA -> $KB"; exit 1; }
test -s "$W/st/disc.bin" || { echo "disc.bin not written"; exit 1; }
grep -q "resumed discovery log" "$W/b.err" || { echo "discovery log not resumed"; cat "$W/b.err"; exit 1; }

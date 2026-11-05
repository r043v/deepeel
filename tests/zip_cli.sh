#!/bin/sh
# A .zip romset archive is accepted as the ROM input: exploring the zipped
# fixture (deflate and stored) gives the same report as the raw ROM.
# usage: zip_cli.sh <deepeel> <rom.gb> <python3>
set -e
D=$1; ROM=$2; PY=$3; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
"$PY" -I - "$ROM" "$W" <<'PYEOF'
import sys, zipfile
rom, out = sys.argv[1], sys.argv[2]
for name, method in (("deflate.zip", zipfile.ZIP_DEFLATED), ("stored.zip", zipfile.ZIP_STORED)):
    with zipfile.ZipFile(out + "/" + name, "w", method) as z:
        z.write(rom, "game.gb")
PYEOF
"$D" explore "$ROM" --text "$W/raw.txt" >/dev/null 2>&1
for z in deflate stored; do
  "$D" explore "$W/$z.zip" --text "$W/$z.txt" >/dev/null 2>&1
  # the report names the input path on some lines; compare the rest
  grep -v -F -e "$ROM" -e "$W" "$W/raw.txt" > "$W/raw.cmp"
  grep -v -F -e "$ROM" -e "$W" "$W/$z.txt" > "$W/$z.cmp"
  cmp -s "$W/raw.cmp" "$W/$z.cmp" || { echo "zip ($z) report differs from raw ROM"; diff "$W/raw.cmp" "$W/$z.cmp" | head; exit 1; }
done
test -s "$W/raw.txt"

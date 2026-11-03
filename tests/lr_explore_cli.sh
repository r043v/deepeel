#!/bin/sh
# deepeel lr explore on the mock core: both sides of the conditional, saturation exit codes.
set -e
D=$1; CORE=$2; ROM=$3
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf '\001\005\004\010\002\020\005\000\002\022\005\000' > "$T/p.bin"
"$D" lr explore "$CORE" "$T/p.bin" --text "$T/t.txt" --map "$T/m.txt" --png "$T/m.png"
grep -q "forks 2" "$T/t.txt"
grep -q "code starts reached: 6 of 12" "$T/t.txt"
grep -q "dropped fork items (queue full): 0" "$T/t.txt"
grep -q "code-natural" "$T/m.txt"
[ "$(head -c 4 "$T/m.png" | tail -c 3)" = "PNG" ]
# a saturated run is reported and exits 3, never silently
set +e
"$D" lr explore "$CORE" "$T/p.bin" --queue 1 --text "$T/t2.txt" >/dev/null
rc=$?
set -e
[ "$rc" = 3 ]
grep -q "dropped fork items (queue full): [1-9]" "$T/t2.txt"
# a core without the extension cannot fork
set +e
DPMOCK_NOTRACE=1 "$D" lr explore "$CORE" "$T/p.bin" 2>/dev/null
rc=$?
set -e
[ "$rc" = 1 ]

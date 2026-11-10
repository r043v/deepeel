#!/bin/sh
set -eu
binary=$1
rom=$2
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
for jobs in 1 2; do
    "$binary" nes explore "$rom" --play 20 --sessions 2 --jobs "$jobs" \
        --map "$dir/nes.png" >"$dir/log" 2>&1
    "$binary" maprender "$rom" "$dir/nes.png" "$dir/again.png" --sys nes
    for chunk in dpCv dpCh dpFi dpMt; do
        "$binary" mapx "$dir/nes.png" "$dir/original" --chunk "$chunk"
        "$binary" mapx "$dir/again.png" "$dir/again" --chunk "$chunk"
        cmp "$dir/original" "$dir/again"
    done
    "$binary" mapx "$dir/nes.png" "$dir/heat" --chunk dpCh
    python3 - "$dir/heat" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
assert data[:4] == b'DPCH'
length = struct.unpack_from('<Q', data, 12)[0]
mask = struct.unpack_from('<I', data, 24)[0]
assert mask & (1 << 5)
assert any(data[28:28 + 4 * length]), 'no played execution heat'
PY
done

#!/bin/sh
set -eu
binary=$1
rom=$2
system=$3
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
if [ "$system" = pce ]; then
    dd if=/dev/zero of="$dir/headered.pce" bs=512 count=1 2>/dev/null
    cat "$rom" >>"$dir/headered.pce"
    rom="$dir/headered.pce"
fi
for jobs in 1 2; do
    if [ "$system" = md ]; then
        "$binary" "$system" explore "$rom" --play-frames 20 --play-sessions 2 --jobs "$jobs" \
            --map "$dir/map.png" >"$dir/log" 2>&1
    else
        "$binary" "$system" explore "$rom" --play 20 --sessions 2 --jobs "$jobs" \
            --map "$dir/map.png" >"$dir/log" 2>&1
    fi
    "$binary" maprender "$rom" "$dir/map.png" "$dir/again.png" --sys "$system"
    for chunk in dpCv dpCh dpFi dpMt; do
        "$binary" mapx "$dir/map.png" "$dir/original" --chunk "$chunk"
        "$binary" mapx "$dir/again.png" "$dir/again" --chunk "$chunk"
        cmp "$dir/original" "$dir/again"
    done
    "$binary" mapx "$dir/map.png" "$dir/heat" --chunk dpCh
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

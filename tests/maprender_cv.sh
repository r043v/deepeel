#!/bin/sh
# J10: maprender .map -> PNG, then PNG (dpCv) -> PNG; the two class vectors must be identical.
set -e
D="$1"; R="$2"; T="$(mktemp -d)"
printf '0 0 3fff code-natural\n1 4000 40ff data-read\n1 4100 7fff padding\n' > "$T/a.map"
"$D" maprender "$R" "$T/a.map" "$T/a.png"
"$D" maprender "$R" "$T/a.png" "$T/b.png"
python3 - "$T/a.png" "$T/b.png" <<'P'
import sys, zlib, struct
def cv(f):
    d = open(f, 'rb').read(); i = 8
    while i < len(d):
        l, = struct.unpack('>I', d[i:i+4]); t = d[i+4:i+8]
        if t == b'zTXt' and d[i+8:i+12] == b'dpCv': return zlib.decompress(d[i+8+6:i+8+l])
        i += 12 + l
a, b = cv(sys.argv[1]), cv(sys.argv[2])
assert a is not None and a == b, "dpCv round trip differs"
P
rm -rf "$T"

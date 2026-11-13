#!/usr/bin/env python3 -I
"""Score a bridge DSY1 dump against a reference one: dsy_cmp.py BRIDGE.dsy REF.dsy [SHIFT] (default -1).

Convention (measured, Tonma 243 frames): the reference dump fn=N (BizHawk, tools/bizhawk_dsy) is byte-identical
to the standalone Mednafen dump fn=N+1, i.e. BizHawk ran one Emulate (the 258-line boot frame) before its first
dump.  The bridge numbers frames like Mednafen, so bridge fn=f is compared with reference fn=f-1 (SHIFT -1).
The stack ($100-$1FF) and byte $6 are ignored; 'sustained' = more than 40 diffs over 20 consecutive frames.
"""
import struct
import sys


def rd(p):
    d = open(p, "rb").read(); o = 4; r = {}
    while o + 15 <= len(d):
        fn, nb, nl = struct.unpack_from("<IHB", d, o); o += 7 + nl
        ln = struct.unpack_from("<I", d, o)[0]; o += 4
        r[fn] = d[o:o + ln]; o += ln
    return r


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    a, t = rd(sys.argv[1]), rd(sys.argv[2])
    sh = int(sys.argv[3]) if len(sys.argv) > 3 else -1
    ds = [(f, sum(1 for i in range(0x200, 0x2000) if i != 6 and a[f][i] != t[f + sh][i])) for f in sorted(a) if f + sh in t]
    first = next((f for f, d in ds if d > 0), None)
    sus = next((ds[i][0] for i in range(len(ds) - 20) if all(d > 40 for _, d in ds[i:i + 20])), None)
    print("frames %d identical %d first diff %s sustained %s" % (len(ds), sum(1 for _, d in ds if d == 0), first, sus))


if __name__ == "__main__":
    main()

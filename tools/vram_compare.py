#!/usr/bin/env python3 -I
"""Compare per-frame VRAM/OAM/LCD registers of a deepeel replay with a reference's.

usage: vram_compare.py DEEPEEL_VRAM REF_VRAM [--runs N] [--show K]

  DEEPEEL_VRAM  DEEPEEL_TAS_VRAM=<file>: per movie vblank VRAM bank 0 (8192) + OAM (160) +
                16 register bytes (LCDC SCY SCX LYC BGP OBP0 OBP1 WY WX)
  REF_VRAM      the same record from a reference (VBAH_VRAM of the VBA harness)

Prints, per area (VRAM tiles $8000-$97FF, VRAM maps $9800-$9FFF, OAM, registers), the first
differing frame with its bytes, how many frames differ, and the runs of differing frames.
Numbers are observations of the two emulators, not hardware measurements.
"""
import sys

REC = 0x2000 + 0xA0 + 16
# the palettes (bytes 4..6 of the register block) are not read from a comparable place in every reference
AREAS = (("tiles $8000-$97FF", 0, 0x1800), ("maps  $9800-$9FFF", 0x1800, 0x2000),
         ("OAM", 0x2000, 0x20A0), ("regs LCDC SCY SCX", 0x20A0, 0x20A3))


def main():
    a = sys.argv[1:]
    show = 16
    if "--show" in a:
        i = a.index("--show"); show = int(a[i + 1]); del a[i:i + 2]
    if len(a) != 2:
        sys.exit(__doc__)
    fa, fb = open(a[0], "rb"), open(a[1], "rb")
    n = min(fa.seek(0, 2), fb.seek(0, 2)) // REC
    fa.seek(0); fb.seek(0)
    runs, cur = [], None
    first = [None] * len(AREAS)
    count = [0] * len(AREAS)
    base = None
    for f in range(n):
        ra, rb = fa.read(REC), fb.read(REC)
        if base is None:
            base = (ra, rb)   # frame 0: boot leftovers differ between emulators (VBA keeps the logo tiles)
        # a byte neither emulator has written since frame 0 is an inherited difference, not a divergence
        ra = bytes(rb[i] if (ra[i] == base[0][i] and rb[i] == base[1][i]) else ra[i] for i in range(0x20A0)) + ra[0x20A0:]
        if ra == rb or all(ra[lo:hi] == rb[lo:hi] for _, lo, hi in AREAS):
            if cur: runs.append(cur); cur = None
            continue
        if cur and cur[1] == f - 1: cur[1] = f
        else:
            if cur: runs.append(cur)
            cur = [f, f]
        for k, (name, lo, hi) in enumerate(AREAS):
            if ra[lo:hi] != rb[lo:hi]:
                count[k] += 1
                if first[k] is None:
                    first[k] = (f, [(lo + i, ra[lo + i], rb[lo + i]) for i in range(hi - lo) if ra[lo + i] != rb[lo + i]])
    if cur: runs.append(cur)
    print("frames compared: %d" % n)
    print("runs of differing frames (start..end): %d, longest %s" % (len(runs), sorted(runs, key=lambda r: r[0] - r[1])[:8]))
    print("first runs of 20+ frames: %s" % [r for r in runs if r[1] - r[0] >= 19][:10])
    for k, (name, lo, hi) in enumerate(AREAS):
        if first[k] is None:
            print("%-44s identical" % name)
            continue
        f, d = first[k]
        print("%-44s first differing frame %d, %d frames differ, %d bytes at first" % (name, f, count[k], len(d)))
        for off, x, y in d[:show]:
            print("    +$%04X  deepeel %02X  reference %02X" % (off, x, y))


if __name__ == "__main__":
    main()

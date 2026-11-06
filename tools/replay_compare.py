#!/usr/bin/env python3 -I
"""Compare a deepeel movie replay with a reference emulator's, frame by frame, and say where it diverges.

usage: replay_compare.py DEEPEEL_RAW REF_RAW [--offset N] [--ref-hash GBH_TRACE] [--show K] [--map MAP.sym]

  DEEPEEL_RAW  DEEPEEL_TAS_RAW=<file> of a deepeel replay: 8 KiB WRAM per movie vblank
  REF_RAW      the reference's per-frame WRAM, same layout (GBH_RAW=<file> of tools/gbhawk_ref,
               or a dump from any other emulator: $C000..$DFFF, 8192 bytes per frame)
  --offset N   reference frames that precede deepeel frame 0 (GBHawk boots through the DMG boot ROM:
               334; VBA-style references start after the boot: 0)
  --show K     bytes listed at the first divergence (default 48)
  --ignore LO-HI  WRAM offsets (hex) left out of the comparison, repeatable; the stack ($0F00-$0FFF,
               $1F00-$1FFF) and the sound page are noisy by nature and ignored by default
               (--ignore none keeps everything)

Prints: frames compared, the first frame whose WRAM differs, which bytes (address, deepeel, reference),
how many frames differ in all, whether the two re-converge, and a per-page histogram of the first diff.
A matching replay prints "identical".  Numbers are observations of the two emulators, not hardware.
"""
import sys, os

def main():
    a = sys.argv[1:]
    off = 0; show = 48; ign = []
    def opt(n, d=None):
        if n in a:
            i = a.index(n); v = a[i + 1]; del a[i:i + 2]; return v
        return d
    off = int(opt("--offset", "0")); show = int(opt("--show", "48"))
    while "--ignore" in a:
        ign.append(opt("--ignore"))
    if len(a) != 2:
        sys.exit(__doc__)
    if not ign:
        ign = ["0f00-0fff", "1f00-1fff"]
    mask = bytearray(b"\x01" * 0x2000)
    for r in ign:
        if r == "none":
            continue
        lo, hi = r.split("-")
        for i in range(int(lo, 16), int(hi, 16) + 1):
            mask[i] = 0
    sa, sb = os.path.getsize(a[0]), os.path.getsize(a[1])
    na, nb = sa // 0x2000, sb // 0x2000
    n = min(na, nb - off)
    print("deepeel %d frames, reference %d frames (offset %d): comparing %d" % (na, nb, off, n))
    if n <= 0:
        sys.exit("nothing to compare")
    fa, fb = open(a[0], "rb"), open(a[1], "rb")
    fb.seek(off * 0x2000)
    first = None; differing = 0; last_diff = None; same_after = 0; hist = {}; bad = []
    for f in range(n):
        x, y = fa.read(0x2000), fb.read(0x2000)
        if x == y:
            if first is not None: same_after += 1
            continue
        d = [i for i in range(0x2000) if x[i] != y[i] and mask[i]]
        if not d:
            if first is not None: same_after += 1
            continue
        differing += 1; last_diff = f; bad.append((f, d))
        if first is None:
            first = f
            print("first divergence at deepeel frame %d (reference frame %d): %d byte(s) differ" % (f, f + off, len(d)))
            for i in d[:show]:
                print("  $%04X  deepeel %02X  reference %02X" % (0xC000 + i, x[i], y[i]))
            for i in d: hist[i >> 8] = hist.get(i >> 8, 0) + 1
            print("  by page: " + " ".join("$%02X00:%d" % (0xC0 + p, c) for p, c in sorted(hist.items())))
    if first is None:
        print("identical (%d frames, ignored ranges: %s)" % (n, ",".join(ign)))
        return
    print("frames differing: %d of %d; last differing frame %d; identical frames after the first divergence: %d"
          % (differing, n, last_diff, same_after))
    print("re-converges" if last_diff < n - 1 else "diverged to the end")
    # runs of consecutive differing frames, each with its first bytes: where a replay drifts and heals
    runs = []
    for f, d in bad:
        if runs and runs[-1][1] == f - 1:
            runs[-1][1] = f; runs[-1][3] = max(runs[-1][3], len(d))
        else:
            runs.append([f, f, d, len(d)])
    print("%d run(s) of differing frames:" % len(runs))
    for lo, hi, d, mx in runs[:40]:
        print("  frames %d..%d (max %d bytes), first bytes: %s" % (lo, hi, mx, " ".join("$%04X" % (0xC000 + i) for i in d[:8])))

main()

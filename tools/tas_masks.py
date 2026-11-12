#!/usr/bin/env python3 -I
"""Convert a TAS movie (.fm2 or BizHawk .bk2, any platform) to the lr_dsy / bizhawk_dsy mask file.

usage: tas_masks.py MOVIE OUT.masks [--skip N]
One byte per frame, bit0 Up 1 Down 2 Left 3 Right 4 B 5 A 6 Select 7 Start (first controller; the PC Engine
II/I/Run buttons map to B/A/Start).  --skip drops N leading frames (a movie that starts from power-on has none).
"""
import sys, zipfile, re

BITS = {"up": 0, "down": 1, "left": 2, "right": 3, "b": 4, "a": 5, "select": 6, "start": 7, "ii": 4, "i": 5, "run": 7}

def fm2(text):
    out = []
    for ln in text.splitlines():
        if not ln.startswith("|"):
            continue
        f = ln.split("|")
        pad = f[2] if len(f) > 2 else ""
        if len(pad) < 8:
            continue
        m = 0
        for ch, bit in zip(pad[:8], (3, 2, 1, 0, 7, 6, 4, 5)):   # R L D U T(Start) S(Select) B A
            if ch not in ". ":
                m |= 1 << bit
        out.append(m)
    return out

def bk2(z):
    log = z.read("Input Log.txt").decode("utf-8", "replace").splitlines()
    key = next(l for l in log if l.startswith("LogKey:"))
    groups = key[len("LogKey:"):].split("|")
    names = []   # button names of player 1, in log order
    for g in groups:
        if not g or g.startswith("#"):
            continue
        for b in g.split(","):
            names.append(b.strip())
    p1 = [(i, n[3:].lower()) for i, n in enumerate(names) if n.startswith("P1 ")]
    out = []
    for l in log:
        if not l.startswith("|"):
            continue
        cells = l.strip("|").split("|")
        flat = "".join(cells)
        m = 0
        for i, n in p1:
            if i < len(flat) and flat[i] not in ". " and n in BITS:
                m |= 1 << BITS[n]
        out.append(m)
    return out

def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    if len(a) < 2:
        sys.exit(__doc__)
    skip = int(sys.argv[sys.argv.index("--skip") + 1]) if "--skip" in sys.argv else 0
    p = a[0]
    if p.lower().endswith(".bk2"):
        frames = bk2(zipfile.ZipFile(p))
    else:
        frames = fm2(open(p, "rb").read().decode("utf-8", "replace"))
    frames = frames[skip:]
    open(a[1], "wb").write(bytes(frames))
    print("%d frames" % len(frames), file=sys.stderr)

main()

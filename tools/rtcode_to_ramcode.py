#!/usr/bin/env python3
# rtcode_to_ramcode.py: bridge deepeel -> m0boy for the code the game runs from RAM.
# deepeel exports the runtime-code archive (rtcode v1, `--rtcode FILE`: one line per
# distinct instruction run from RAM/VRAM/HRAM, `pc bytes hits rom_index producer`);
# m0boy's `--ram-code` still consumes the older variant format
# (`ram ADDR HEXBYTES off,off2[e]...`).  This script converts one to the other.
# Exported: every WRAM/HRAM instruction run deepeel observed on a natural path —
# the original --ram-code contract ("code atteint par un chemin naturel"), which
# includes the game's runtime-built routines (no ROM source) as long as the bytes
# were really executed, never guessed.  m0boy only enters a variant when the RAM
# holds exactly these bytes, so a variant that does not match costs one compare.
# VRAM runs (pc < 0xC000) are dropped: m0boy only accepts WRAM/HRAM and the VRAM
# entries of a GB are transfer artifacts, not code.
#   usage: rtcode_to_ramcode.py RTCODE_FILE RAMCODE_FILE
import sys

rows = []
for line in open(sys.argv[1]):
    if line.startswith("#") or not line.strip():
        continue
    p = line.split()
    pc, b = int(p[0], 16), bytes.fromhex(p[1])
    if 0xC000 <= pc < 0xE000 or pc >= 0xFF80:
        rows.append((pc, b))   # WRAM C000-DFFF and HRAM FF80-FFFE only

attached = sorted(rows)
runs, cur = [], None
for pc, b in attached:
    if cur and pc == cur[0] + len(cur[1]):
        cur[1] += b
    else:
        if cur: runs.append(cur)
        cur = [pc, b]
if cur: runs.append(cur)

with open(sys.argv[2], "w") as f:
    f.write("# converted from deepeel rtcode v1 by tools/rtcode_to_ramcode.py: every observed WRAM/HRAM run\n")
    for addr, b in runs:
        offs = ",".join("%xe" % o for o in range(0, len(b)))   # every start is an entry; m0boy gates on the exact bytes
        f.write("ram %04x %s %s\n" % (addr, b.hex(), offs))
print("%d variants, %d bytes" % (len(runs), sum(len(b) for _, b in runs)))

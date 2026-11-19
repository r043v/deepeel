#!/usr/bin/env python3
"""Compare a deepeel .cov with a reference .cov (flag 1 = reference code, 8 = reference data; from ida_asm_cov.py or
s1disasm_ref.py --ref-cov).  usage: cov_compare.py ref.cov ours.cov [--top N]
Prints: reference code executed (flag 1) / proven only (flag 4) / missing, data read, and the largest missing code runs.
Observation of the tool vs a community disassembly; not a hardware measure."""
import sys
def load(p):
    d = {}
    for l in open(p):
        if l[0] == '#': continue
        q = l.split()
        if len(q) >= 3: d[int(q[0], 16) * 0x4000 + (int(q[1], 16) & 0x3fff)] = int(q[2], 16)
    return d
ref, our = load(sys.argv[1]), load(sys.argv[2])
top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
code = [k for k, f in ref.items() if f & 1]
ex = sum(1 for k in code if our.get(k, 0) & 1); pr = sum(1 for k in code if not our.get(k, 0) & 1 and our.get(k, 0) & 4)
print("reference code %d bytes: executed %d (%.1f%%), proven only %d, missing %d (%.1f%%)" % (len(code), ex, 100.0 * ex / len(code), pr, len(code) - ex - pr, 100.0 * (len(code) - ex - pr) / len(code)))
data = [k for k, f in ref.items() if f & 8 and not f & 1]
print("reference data %d bytes: read/classified by us %d (%.1f%%)" % (len(data), sum(1 for k in data if our.get(k, 0) & 8), 100.0 * sum(1 for k in data if our.get(k, 0) & 8) / max(len(data), 1)))
miss = sorted(k for k in code if not our.get(k, 0) & 5)
runs = []; s = p = None
for k in miss:
    if s is None: s = p = k
    elif k - p <= 2: p = k
    else: runs.append((s, p + 1)); s = p = k
if s is not None: runs.append((s, p + 1))
runs.sort(key=lambda r: r[0] - r[1])
print("largest missing code runs (start, bytes):")
for a, b in runs[:top]: print("  %06X %5d" % (a, b - a))

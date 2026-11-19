#!/usr/bin/env python3
"""Extract IDA 5.x .idb ROM classification (code/data/unknown per byte) and compare it with a deepeel .cov.
usage: ida_idb_code.py <file.idb> [<file.cov> [--missing]]   (ROM = segment starting at 0 ; MD flat ROM)
IDB ID1 layout (Va4): u32 magic, u16 nseg, u16 pad, nseg x (start,end) u32, pad to +0x2000, then one u32 per byte:
low byte = byte value, bits 8.. = flags (MS_CLS 0x600: 0x600 code head, 0x400 data, 0x200 tail, 0 unknown)."""
import struct, sys, collections
def load(path):
    d = open(path, 'rb').read()
    o = d.find(b'Va4\0')
    nseg = struct.unpack_from('<H', d, o + 4)[0]
    segs = [struct.unpack_from('<II', d, o + 8 + 8 * i) for i in range(nseg)]
    base = o + 0x2000
    rom_end = next(e for s, e in segs if s == 0)
    n = rom_end if rom_end < (1 << 24) else rom_end
    fl = struct.unpack_from('<%dI' % n, d, base)
    return segs, fl
def code_bytes(fl):
    """bytes belonging to an instruction: a code head plus its tail bytes up to the next head/unknown"""
    n = len(fl); out = bytearray(n); i = 0
    while i < n:
        c = fl[i] & 0x600
        if c == 0x600:
            out[i] = 1; j = i + 1
            while j < n and (fl[j] & 0x600) == 0x200: out[j] = 1; j += 1
            i = j
        else: i += 1
    return out
if __name__ == '__main__':
    segs, fl = load(sys.argv[1])
    cb = code_bytes(fl)
    print('segments', [(hex(s), hex(e)) for s, e in segs])
    print('IDA code bytes in ROM: %d  data: %d  unknown: %d' % (sum(cb), sum(1 for f in fl if (f & 0x600) == 0x400), sum(1 for f in fl if (f & 0x600) == 0)))
    if len(sys.argv) > 2:
        cov = {}
        for l in open(sys.argv[2]):
            p = l.split()
            if len(p) == 4 and p[0] != '#': cov[int(p[0], 16) * 0x4000 + (int(p[1], 16) & 0x3FFF)] = int(p[2], 16)
        ex = sum(1 for i, v in enumerate(cb) if v and cov.get(i, 0) & 1)
        st = sum(1 for i, v in enumerate(cb) if v and not cov.get(i, 0) & 1 and cov.get(i, 0) & 4)
        tot = sum(cb)
        print('IDA code covered: executed %d (%.1f%%) + static/spec %d (%.1f%%) / %d' % (ex, 100.0 * ex / tot, st, 100.0 * st / tot, tot))
        extra = sum(1 for i, v in cov.items() if (v & 1) and i < len(cb) and not cb[i])
        print('executed by deepeel but not code in IDA: %d' % extra)
        if '--missing' in sys.argv:
            runs = []; i = 0
            while i < len(cb):
                if cb[i] and not cov.get(i, 0) & 5:
                    j = i
                    while j < len(cb) and cb[j] and not cov.get(j, 0) & 5: j += 1
                    runs.append((i, j - i)); i = j
                else: i += 1
            runs.sort(key=lambda r: -r[1])
            print('missing runs: %d, bytes %d' % (len(runs), sum(r[1] for r in runs)))
            for a, l in runs[:40]: print('  %06X  %5d' % (a, l))

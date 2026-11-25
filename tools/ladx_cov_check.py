#!/usr/bin/env python3
"""Check the classes of a deepeel coverage.bin against a LADX-Disassembly reference map (ladx_disasm_ref.py):
per class, the share the disassembly calls code, data or padding, and the recall of the reference code.
usage: ladx_cov_check.py ref.map coverage.bin [...]
The reference has known gaps (instructions without an address comment read as data): check disagreements in the
sources.  Observation of the tool vs a community disassembly; not a hardware measure."""
import sys, struct, collections
def refmap(p, n):
    v = ['inconnu'] * n
    for l in open(p):
        if l[0] == '#' or not l.strip(): continue
        b, s, e, c = l.split()[:4]; b = int(b)
        for i in range(b * 0x4000 + (int(s, 16) & 0x3FFF), b * 0x4000 + (int(e, 16) & 0x3FFF) + 1): v[i] = c
    return v
def cov(p):
    b = open(p, 'rb').read(); n = struct.unpack_from('<I', b, 4)[0]; u = b[8:8 + n]; q = 8 + n; sec = {}
    while q + 8 <= len(b):
        t, sz = struct.unpack_from('<4sI', b, q); w = 4 if t in (b'SCR1', b'HIT1') else 1; sec[t] = b[q + 8:q + 8 + sz * w]; q += 8 + sz * w
    return n, u, sec
for path in sys.argv[2:]:
    n, u, sec = cov(path); ref = refmap(sys.argv[1], n); z = bytes(n)
    inf, prv, x = sec.get(b'INF1', z), sec.get(b'PRV1', z), sec.get(b'XPO1', z)
    t = collections.defaultdict(collections.Counter)
    for i in range(n):
        f = u[i]
        k = ('1 code observé' if f & 1 else '2 données observées' if f & 28 else '3 statique prouvé' if f & 32 else
             '4 autre version, code' if x[i] == 1 else '5 autre version, données' if x[i] == 2 else '6 suspect forcé' if f & 2 else
             '7 suspect code' if prv[i] or inf[i] == 1 else '8 suspect inféré' if inf[i] else '9 inconnu')
        t[k][ref[i]] += 1
    cols = sorted(set(ref)); tot = collections.Counter(ref)
    print('==', path, ' référence:', dict(tot))
    print('%-28s %8s ' % ('classe deepeel', 'octets') + ' '.join('%9s' % c[:9] for c in cols))
    for k in sorted(t):
        m = sum(t[k].values()); print('%-28s %8d ' % (k, m) + ' '.join('%8.1f%%' % (100 * t[k][c] / m) for c in cols))
    rc = tot['code-static']; got = lambda ks: 100 * sum(t[k]['code-static'] for k in t if k[0] in ks) / max(rc, 1)
    print('rappel du code de référence : observé %.1f%%, + prouvé %.1f%%, + autre version %.1f%%, + forcé %.1f%%' % (got('1'), got('13'), got('134'), got('1346')))

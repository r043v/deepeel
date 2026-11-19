#!/usr/bin/env python3
"""Reference coverage from an IDA-generated 68000 listing (no addresses): writes a deepeel .cov (flag 1 = instruction byte).
usage: ida_asm_cov.py <listing.asm> <rom.md> <out.cov> [--deepeel build/deepeel]
Anchors: labels whose name ends in the hex address (loc_1A2B, sub_..., byte_..., word_..., dword_..., off_..., unk_...).
Between anchors the position advances by instruction lengths (taken from `deepeel md dis`, i.e. our own decoder) and by the
size of dc.*/dcb.*/ds.*/even lines.  Every anchor re-synchronises the position; a mismatch is reported (count, first ones)."""
import re, subprocess, sys
lst, rom, out = sys.argv[1:4]
deepeel = sys.argv[sys.argv.index('--deepeel') + 1] if '--deepeel' in sys.argv else 'build/deepeel'
lines = open(lst, encoding='latin-1').read().split('\n')
LAB = re.compile(r'^([A-Za-z_.][\w.$]*):?')
ANCH = re.compile(r'^(?:loc|locret|sub|byte|word|dword|off|unk|nullsub|def|stru|asc|a[A-Z]\w*)?_?([0-9A-F]{4,6}):?(?=\s|$)')
def split_ops(s):
    ops, cur, q = [], '', None
    i = 0
    while i < len(s):
        c = s[i]
        if q:
            cur += c
            if c == q:
                if i + 1 < len(s) and s[i + 1] == q: cur += s[i + 1]; i += 1
                else: q = None
        elif c in "'\"": q = c; cur += c
        elif c == ';': break
        elif c == ',': ops.append(cur.strip()); cur = ''
        else: cur += c
        i += 1
    if cur.strip(): ops.append(cur.strip())
    return ops
def item_len(o, w):
    if o[:1] in "'\"" and o[-1:] == o[:1] and len(o) >= 2:
        return len(o[1:-1].replace("''", "'").replace('""', '"')) * 1
    return w
def eval_num(t):
    t = t.strip()
    try: return int(t[1:], 16) if t.startswith('$') else int(t, 0)
    except Exception: return None
# pass 1: parse lines into (anchor, kind, size/None)
recs = []   # (anchor_addr or None, kind, payload)
for ln in lines:
    if not ln.strip() or ln.lstrip().startswith(';') or ln.startswith('\t\t\t\t\t;'): continue
    anchor = None; body = ln
    if ln[0] not in ' \t':
        m = re.match(r'^([A-Za-z_.][\w.$]*):?', ln)
        if m:
            name = m.group(1)
            ma = re.search(r'_([0-9A-F]{4,6})$', name)
            if ma and name.split('_')[0] in ('loc', 'locret', 'sub', 'byte', 'word', 'dword', 'off', 'unk', 'nullsub', 'def', 'stru'):
                anchor = int(ma.group(1), 16)
            body = ln[m.end():]
    body = body.strip()
    if not body or body.startswith(';'):
        if anchor is not None: recs.append((anchor, 'label', 0))
        continue
    mn = body.split(None, 1)
    op = mn[0].lower(); rest = mn[1] if len(mn) > 1 else ''
    if op in ('equ', 'macro', 'endm', 'include', 'org', 'rsset', 'rs.b', 'rs.w', 'rs.l', 'section', '='): 
        if anchor is not None: recs.append((anchor, 'label', 0))
        continue
    mm = re.match(r'^(dc|dcb|ds)\.([bwl])$', op)
    if mm:
        w = {'b': 1, 'w': 2, 'l': 4}[mm.group(2)]
        ops = split_ops(rest)
        if mm.group(1) == 'dc': n = sum(item_len(o, w) for o in ops)
        elif mm.group(1) == 'dcb': c = eval_num(ops[0]) if ops else None; n = (c or 0) * w
        else: c = eval_num(ops[0]) if ops else None; n = (c or 0) * w
        recs.append((anchor, 'data', n)); continue
    if op in ('even', 'align', 'cnop'): recs.append((anchor, 'pad', rest)); continue
    recs.append((anchor, 'insn', body))
# pass 2: group runs of instructions per anchor-resync, ask the decoder for lengths
cov = bytearray(0x400000)
pos = 0; mism = 0; first = []; pending = []   # (start, n)
def flush_insns(start, n):
    if n <= 0: return start
    r = subprocess.run([deepeel, 'md', 'dis', rom, '%X' % start, str(n)], capture_output=True, text=True).stdout.split('\n')
    addrs = [int(l[:6], 16) for l in r if re.match(r'^[0-9A-F]{6}  ', l)]
    if len(addrs) < n: return None
    for a, b in zip(addrs, addrs[1:] + [None]):
        if b is None: break
        for k in range(a, b): cov[k] = 1
    # the last instruction's length: decode n+1 and use the next address
    r2 = subprocess.run([deepeel, 'md', 'dis', rom, '%X' % start, str(n + 1)], capture_output=True, text=True).stdout.split('\n')
    a2 = [int(l[:6], 16) for l in r2 if re.match(r'^[0-9A-F]{6}  ', l)]
    if len(a2) < n + 1: return None
    for k in range(a2[n - 1], a2[n]): cov[k] = 1
    return a2[n]
i = 0; N = len(recs); regions = 0
while i < N:
    a, kind, pl = recs[i]
    if a is not None:
        if a != pos:
            mism += 1
            if len(first) < 8: first.append((hex(pos), hex(a)))
        pos = a
    if kind == 'insn':
        j = i; n = 0
        while j < N and recs[j][1] == 'insn' and (j == i or recs[j][0] is None): n += 1; j += 1
        e = flush_insns(pos, n)
        if e is None: e = pos + 2 * n
        pos = e; i = j; regions += 1; continue
    if kind == 'data': pos += pl
    elif kind == 'pad':
        if pos & 1: pos += 1
    i += 1
code = sum(cov)
print('regions %d, resync mismatches %d %s' % (regions, mism, first))
print('reference code bytes: %d of %d' % (code, 0x80000))
with open(out, 'w') as f:
    f.write('# reference coverage from %s (IDA listing), flag 1 = instruction byte\n' % lst.split('/')[-1])
    for i in range(0x80000):
        if cov[i]: f.write('%02x %04x 1 1\n' % (i >> 14, i & 0x3FFF))

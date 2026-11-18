#!/usr/bin/env python3
"""Reference check for Sonic 1 (REV00) against the s1disasm listing (information only, not a coverage source).

Build the ROM in a COPY of s1disasm with `Revision = 0` (bit-identical to the REV00 dump), keep sonic.lst, then:
    tools/s1disasm_ref.py sonic.lst deepeel.cov
Classes per ROM byte come from the assembler listing: an instruction line is code, dc/binclude/even is data;
the Z80 DAC driver block is left unclassified. Output: reference class x deepeel flag (exec / data-read / unseen).
Observation of the tool vs a community disassembly; not a hardware measure."""
import re, sys, collections
rx=re.compile(r'^(?:\(\d+\))?\s*(\d+)/\s*([0-9A-F]+) :(.{21})(.*)$')
DATA=re.compile(r'^(dc(\.[bwlq])?|dcb(\.[bwlq])?|incbin|binclude|ds(\.[bwlq])?|even|cnop|align|rorg|org|dc)$',re.I)
ents=[];z80=False
for ln in open(sys.argv[1],errors='replace'):
    m=rx.match(ln.rstrip('\n'))
    if not m: continue
    addr=int(m.group(2),16); byt=m.group(3).strip(); src=m.group(4)
    if not re.fullmatch(r'[0-9A-F ]*',byt): byt=''
    toks=src.replace('\t',' ').split()
    if src[:1] not in ' \t' and toks: toks=toks[1:]
    elif src[:1] not in ' \t' and toks and not toks[0].endswith(':'):
        # label without colon, or mnemonic at col 0
        if len(toks)>1 and not re.match(r'^[a-z]',toks[0],re.I)==None: pass
    mn=toks[0].lower() if toks else ''
    if mn=='cpu' and len(toks)>1: z80 = toks[1].lower().startswith('z80')
    if mn in('phase',): z80=True
    if mn in('dephase',): z80=False
    if not byt and not (DATA.match(mn) and not mn.startswith('ds')): continue
    if z80 or mn in('cpu','phase','dephase','!org','org','rorg'): continue
    ents.append((addr,byt,mn,ln))
# class per ROM byte: 0 none, 1 code, 2 data
cls=bytearray(0x80000)
prev=None
rows=[];lk=0
for a,b,m,_ in ents:
    if a>=0x80000 or a<lk: continue
    rows.append((a,b,m)); lk=a
last=-1
for i,(a,b,m) in enumerate(rows):
    if a>=0x80000: continue
    # length: distance to the next entry with a greater address
    j=i+1
    while j<len(rows) and rows[j][0]<=a: j+=1
    end=rows[j][0] if j<len(rows) else 0x80000
    if end>0x80000: end=0x80000
    if not b and not DATA.match(m): continue
    if c0 := (b and not DATA.match(m)): end=min(end,a+len(b.replace(' ',''))//2)
    if re.match(r'^ds(\.[bwlq])?$',m): continue
    if m in('even','cnop','align','org','rorg') : c=2
    elif DATA.match(m): c=2
    else: c=1 if b else 0
    if c and end>a:
        for k in range(a,end):
            if cls[k]==0: cls[k]=c
pass

fl = bytearray(0x80000)
for l in open(sys.argv[2]):
    if l[0] == "#": continue
    p = l.split()
    i = int(p[0], 16) * 0x4000 + (int(p[1], 16) & 0x3fff)
    if i < 0x80000: fl[i] |= int(p[2], 16)
C = collections.Counter()
for i in range(0x80000):
    f = fl[i]
    C[(cls[i], "exec" if f & 1 else "data-read" if f & 8 else "unseen")] += 1
for (c, d), n in sorted(C.items()): print("%-9s %-10s %7d" % (("ref-none", "ref-CODE", "ref-data")[c], d, n))
code = sum(n for (c, d), n in C.items() if c == 1)
print("code reference: %d octets, executed by deepeel: %d (%.1f%%)" % (code, C[(1, "exec")], 100.0 * C[(1, "exec")] / max(code, 1)))

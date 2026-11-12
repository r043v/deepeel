#!/usr/bin/env python3 -I
"""Compare two CPU instruction traces and report the first divergence (any platform, any core).

usage: trace_cmp.py A.txt B.txt [--context N] [--align K] [--names NAMEA,NAMEB]

A trace is text, one event per line; only the lines are used:
  I <pc> a=.. x=.. y=.. s=.. p=.. [t=..]     an executed instruction (hex fields, the time is ignored)
  R/W <addr>[=<val>] ...                      a register access (kept as context, printed before the divergence)
Other lines are ignored.  The two streams start at different points (each core cuts its frames elsewhere): they
are aligned on the first K consecutive instructions (default 12) that both contain, then compared one by one
(pc and registers).  The first difference is printed with the CONTEXT instructions before it and the last
register accesses of each side, which is usually where the cause is (the instruction shown is the effect).
"""
import re
import sys

INS = re.compile(r"^I ([0-9a-fA-F]+)((?: [a-z]+=[0-9a-fA-F]+)*)")


def load(path):
    ins, acc, accat = [], [], []   # instructions (tuple), accesses (text), instruction index each access follows
    with open(path, errors="replace") as f:
        for line in f:
            m = INS.match(line)
            if m:
                regs = tuple(sorted((k, v) for k, v in (x.split("=") for x in m.group(2).split()) if k != "t"))
                ins.append((m.group(1).lower(), regs))
            elif line[:2] in ("R ", "W ") or line.startswith("RV"):
                acc.append(line.strip())
                accat.append(len(ins))
    return ins, acc, accat


def fmt(i):
    return "%s %s" % (i[0], " ".join("%s=%s" % kv for kv in i[1]))


def align(a, b, k, limit=60000):
    """(ia, ib) such that a[ia:ia+k] == b[ib:ib+k], smallest ia + ib."""
    keys = {}
    for j in range(min(len(b) - k, limit)):
        keys.setdefault(tuple(b[j:j + k]), j)
    for i in range(min(len(a) - k, limit)):
        j = keys.get(tuple(a[i:i + k]))
        if j is not None:
            return i, j
    return None


def main():
    argv = sys.argv[1:]
    def opt(n, d):
        if n in argv:
            i = argv.index(n); v = argv[i + 1]; del argv[i:i + 2]; return v
        return d
    ctx = int(opt("--context", 8)); k = int(opt("--align", 12)); names = opt("--names", "A,B").split(",")
    if len(argv) != 2:
        sys.exit(__doc__)
    (a, aa, aat), (b, ba, bat) = load(argv[0]), load(argv[1])
    print("%s: %d instructions, %s: %d instructions" % (names[0], len(a), names[1], len(b)))
    al = align(a, b, k)
    if not al:
        sys.exit("no common window of %d instructions: the traces never meet" % k)
    ia, ib = al
    print("aligned: %s from #%d, %s from #%d" % (names[0], ia, names[1], ib))
    n = min(len(a) - ia, len(b) - ib)
    for d in range(n):
        if a[ia + d] != b[ib + d]:
            print("\nFIRST DIVERGENCE after %d identical instructions" % d)
            for q in range(max(0, d - ctx), d):
                print("  = %s" % fmt(a[ia + q]))
            print("  %s: %s\n  %s: %s" % (names[0], fmt(a[ia + d]), names[1], fmt(b[ib + d])))
            for nm, ac, at, pos in ((names[0], aa, aat, ia + d), (names[1], ba, bat, ib + d)):
                last = [ac[j] for j in range(len(ac)) if at[j] <= pos][-6:]
                print("last accesses before it, %s:\n    %s" % (nm, "\n    ".join(last) if last else "(none)"))
            return 1
    print("identical over %d instructions" % n)
    return 0


if __name__ == "__main__":
    sys.exit(main())

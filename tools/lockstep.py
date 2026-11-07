#!/usr/bin/env python3
"""Lockstep comparison of deepeel against the VBA-rr V7 harness.

Runs both emulators on the same movie, logs every executed instruction
(VBA: ticks pc a f bc de hl sp; deepeel: cycles pc a f bc de hl sp ime),
aligns the two traces and reports the first instruction where the program
counter or the registers differ, and every change of the time offset
(deepeel cycles - 4 * VBA ticks) on the way.

  tools/lockstep.py --rom ROM --movie MOVIE.vbm --vbah ./vbah7 \
      --t0 625900 --t1 638000 [--mask-a 2800-283f] [--env K=V ...]

The window is given in VBA ticks (1 tick = 4 deepeel cycles).  The numbers
are tool observations, not hardware measurements.
"""
import argparse, os, subprocess, sys, tempfile, threading

def read_fifo_window(fifo, lo, hi, out):
    with open(fifo) as f, open(out, "w") as o:
        for line in f:
            t = line.split(None, 1)
            if not t:
                continue
            c = int(t[0])
            # deepeel's cycle counter is 32 bits wide: compare modulo 2**32
            if (c - lo) & 0xFFFFFFFF <= (hi - lo) & 0xFFFFFFFF:
                o.write(line)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rom", required=True)
    ap.add_argument("--movie", required=True)
    ap.add_argument("--vbah", required=True, help="VBA V7 harness binary (VBAH_PCT/VBAH_T0/VBAH_T1 hooks)")
    ap.add_argument("--deepeel", default=os.path.join(os.path.dirname(__file__), "..", "build", "deepeel"))
    ap.add_argument("--t0", type=int, required=True, help="window start, VBA ticks")
    ap.add_argument("--t1", type=int, required=True, help="window end, VBA ticks")
    ap.add_argument("--mask-a", action="append", default=[], metavar="LO-HI",
                    help="pc range (hex) where A/F differences are ignored (e.g. joypad reads)")
    ap.add_argument("--env", action="append", default=[], help="K=V passed to deepeel (e.g. DEEPEEL_TAS_HALTSND=0)")
    ap.add_argument("--keep", help="directory where the two traces are kept")
    ap.add_argument("--context", type=int, default=6, help="instructions shown before/after the divergence")
    a = ap.parse_args()

    work = a.keep or tempfile.mkdtemp(prefix="lockstep.")
    os.makedirs(work, exist_ok=True)
    vtr, dtr, fifo = (os.path.join(work, n) for n in ("vba.txt", "deepeel.txt", "ins.fifo"))
    if os.path.exists(fifo):
        os.remove(fifo)
    os.mkfifo(fifo)

    env = dict(os.environ, VBAH_PCT=vtr, VBAH_T0=str(a.t0), VBAH_T1=str(a.t1))
    subprocess.run([a.vbah, a.rom, a.movie], env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=600)

    th = threading.Thread(target=read_fifo_window, args=(fifo, a.t0 * 4 - 2000, a.t1 * 4 + 2000, dtr))
    th.start()
    denv = dict(os.environ, DEEPEEL_TAS_INSTR=fifo, DEEPEEL_TAS_INSTR_WIN="%d-%d" % ((a.t0 * 4 - 2000) & 0xFFFFFFFF, (a.t1 * 4 + 2000) & 0xFFFFFFFF))
    for kv in a.env:
        k, v = kv.split("=", 1)
        denv[k] = v
    state = os.path.join(work, "state")
    subprocess.run(["rm", "-rf", state])
    subprocess.run([a.deepeel, "explore", a.rom, "--play", "--tas-solo", "--jobs", "1",
                    "--state-dir", state, "--tas", a.movie], env=denv,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1200)
    th.join(timeout=5)

    v = [l.split() for l in open(vtr)]
    d = [l.split() for l in open(dtr)]
    if not v or not d:
        sys.exit("empty trace (vba %d lines, deepeel %d lines)" % (len(v), len(d)))

    masks = []
    for m in a.mask_a:
        lo, hi = m.split("-")
        masks.append((int(lo, 16), int(hi, 16)))

    def eq(x, y):
        # VBA keeps the low nibble of F after POP AF; deepeel stores flags decomposed
        x = x[:3] + ["%02x" % (int(x[3], 16) & 0xF0)] + x[4:]
        y = y[:3] + ["%02x" % (int(y[3], 16) & 0xF0)] + y[4:]
        if x[1:8] == y[1:8]:
            return True
        if x[1] == y[1] and x[4:8] == y[4:8]:
            pc = int(x[1], 16)
            return any(lo <= pc <= hi for lo, hi in masks)
        return False

    best = (0, -1)
    for k, x in enumerate(d):
        if eq(x, v[0]):
            j, i = k, 0
            while i < len(v) and j < len(d) and eq(v[i], d[j]):
                i += 1
                j += 1
            if i > best[0]:
                best = (i, k)
            if i == len(v):
                break
    n, k = best
    if k < 0:
        sys.exit("no alignment found: the first VBA instruction %s is absent from the deepeel window" % v[0][:8])
    print("window %d..%d ticks: %d VBA instructions, %d deepeel; aligned at deepeel line %d" %
          (a.t0, a.t1, len(v), len(d), k))
    prev = None
    for i in range(n):
        off = (int(d[k + i][0]) - 4 * int(v[i][0]) + 0x80000000) % 0x100000000 - 0x80000000  # modulo 2**32
        if off != prev:
            print("  offset %+d cycles from instruction %d (pc %s, VBA tick %s)" % (off, i, v[i][1], v[i][0]))
            prev = off
    if n == len(v):
        print("IDENTICAL over the whole window (%d instructions)" % n)
        return 0
    print("DIVERGENCE at instruction %d of %d" % (n, len(v)))
    lo = max(0, n - a.context)
    for i in range(lo, min(len(v), n + a.context)):
        mark = ">>" if i == n else "  "
        dd = d[k + i] if k + i < len(d) else None
        print("%s v %-8s %s %s | d %-9s %s" % (mark, v[i][0], " ".join(v[i][1:8]),
              "", dd[0] if dd else "-", " ".join(dd[1:8]) if dd else "-"))
    print("traces kept in", work)
    return 1

if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Frame-by-frame full-state check of the m0boy GB/GBC core against libgambatte.

Produces (or reuses) two DSY1 streams for the same BK2 movie:
  native    deepeel explore --replay-only --dsy      (the m0boy core under test)
  reference tools/gambatte_dsy.py                    (libgambatte of the movie's BizHawk)
and compares every shared block of every frame (regs, wram, wram_banks, vram,
oam, hram, sram, ie).  For each block it reports the first frame that differs
once the boot is over, how many frames differ, and the first bytes with their
meaning (CPU register, $Cxxx/$Dxxx address, HRAM $FFxx, VRAM $8xxx, OAM $FExx).
The first diverging frame is also shown in detail with the previous frame's
state, which is what a bisection needs.  Exit status 1 on divergence.

The power-on RAM pattern differs between the two cores until the boot ROM has
cleared it, so frames up to --boot (default 400) are only reported as noise
unless --boot 0 is given.

Example (Pokemon Crystal, mrwint, BizHawk 2.3):
  python3 tools/gb_sync_check.py --deepeel build-cgb-release/deepeel \\
      --lib output/bizhawk-2.3-src/libgambatte/libgambatte23.so --bizhawk-23 \\
      --rom crystal.gbc --bios cgb.bin --movie film.bk2 --frames 12000 --work DIR
"""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tasmeta

HERE = Path(__file__).resolve().parent
NOISY = ('wram_banks', 'vram', 'oam', 'hram')
REGS = ("pc", "sp", "a", "f", "bc", "de", "hl")


def frames(path, limit=None):
    with open(path, "rb") as f:
        if f.read(4) != b"DSY1":
            raise ValueError(f"{path}: not a DSY1 file")
        n = 0
        while True:
            head = f.read(6)
            if len(head) < 6 or (limit is not None and n >= limit):
                return
            number, count = struct.unpack("<IH", head)
            blocks = {}
            for _ in range(count):
                name = f.read(f.read(1)[0]).decode()
                size = struct.unpack("<I", f.read(4))[0]
                blocks[name] = f.read(size)
            n += 1
            yield number, blocks


def describe(name, index, vram_bank_size=0x2000):
    if name == "regs":
        # packed "<HHBBHHH": pc sp a f bc de hl
        spans = [("pc", 0, 2), ("sp", 2, 4), ("a", 4, 5), ("f", 5, 6), ("bc", 6, 8), ("de", 8, 10), ("hl", 10, 12)]
        for reg, lo, hi in spans:
            if lo <= index < hi:
                return reg
        return f"+{index:x}"
    if name == "wram":
        return f"${0xC000 + index:04X}"
    if name == "wram_banks":
        return f"bank{index // 0x1000}:${0xC000 + index % 0x1000:04X}"
    if name == "hram":
        return f"${0xFF80 + index:04X}"
    if name == "vram":
        return f"vram{index // vram_bank_size}:${0x8000 + index % vram_bank_size:04X}"
    if name == "oam":
        return f"${0xFE00 + index:04X}"
    return f"+{index:04x}"


def diffs(a, b):
    return [i for i, (x, y) in enumerate(zip(a, b)) if x != y]


def show(name, a, b, limit, idx):
    out = [f"{describe(name, i)}={a[i]:02x}/{b[i]:02x}" for i in idx[:limit]]
    return len(idx), " ".join(out)


def compare(native, ref, boot, show_n, ignore):
    first, count = {}, {}
    base, equal_once = {}, set()
    prev = None
    detail = {}
    total = 0
    for (n, a), (m, b) in zip(frames(native), frames(ref)):
        total = n
        if n != m:
            print(f"frame numbering mismatch native={n} reference={m}")
            break
        bad = []
        for name in a.keys() & b.keys():
            if name.endswith("_crc") and a[name] == b[name]:
                equal_once.add(name)
        for name in sorted(a.keys() & b.keys()):
            if name in ignore or name == "wram" or a[name] == b[name]:   # wram is a view of wram_banks
                continue
            idx = diffs(a[name], b[name])
            if name.endswith("_crc"):
                # digest mode: a bank neither core has written differs by its power-on pattern for ever; it counts
                # only from the first frame it matched
                if name not in equal_once:
                    continue
                idx = [0]
            elif name in NOISY:
                # a byte neither core has written since the first frame differs only by its power-on pattern
                ba, bb = base.setdefault(name, (a[name], b[name]))
                idx = [i for i in idx if not (a[name][i] == ba[i] and b[name][i] == bb[i])]
            if not idx:
                continue
            count[name] = count.get(name, 0) + 1
            if n > boot and name not in first:
                first[name] = n
                detail[name] = (n, a[name], b[name], prev, idx)
        prev = (n, {k: (a.get(k), b.get(k)) for k in ("regs", "hram") if k in a and k in b})
        if n % 2000 == 0:
            print(f"  ... frame {n}: {len(first)} block(s) diverged so far", file=sys.stderr)
    return total, first, count, detail


def rtc_offset(movie):
    import json, zipfile
    with zipfile.ZipFile(movie) as z:
        return json.loads(z.read("SyncSettings.json"))["o"].get("RTCDivisorOffset", 0)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--deepeel", type=Path, required=True)
    p.add_argument("--lib", type=Path, required=True, help="libgambatte.so matching the movie's BizHawk")
    p.add_argument("--rom", type=Path, required=True)
    p.add_argument("--bios", type=Path, required=True)
    p.add_argument("--movie", type=Path, required=True)
    p.add_argument("--frames", type=int, default=3000)
    p.add_argument("--work", type=Path, required=True, help="directory for the two .dsy files (large: ~90 KiB/frame)")
    p.add_argument("--bizhawk-23", action="store_true")
    p.add_argument("--bizhawk-232", action="store_true", help="libgambatte of BizHawk 2.3.2 (cycle-based RTC)")
    p.add_argument("--digest", action="store_true", help="long scan: CRCs of the large blocks, ~150 bytes/frame")
    p.add_argument("--from-frame", type=int, default=0, help="detailed window: write frames >= N only")
    p.add_argument("--reuse", action="store_true", help="compare existing native.dsy/ref.dsy without regenerating")
    p.add_argument("--boot", type=int, default=400, help="frames ignored as boot-time RAM noise")
    p.add_argument("--show", type=int, default=12, help="bytes listed per block")
    p.add_argument("--ignore", action="append", default=[], metavar="BLOCK", help="skip a block (e.g. sram)")
    args = p.parse_args()

    emu = tasmeta.require_emulator(args.deepeel, args.rom, args.movie)   # mandatory: the TASVideos API names the emulator
    tasmeta.require_lib_for(emu, args.lib)
    args.work.mkdir(parents=True, exist_ok=True)
    native, ref = args.work / "native.dsy", args.work / "ref.dsy"
    if not args.reuse:
        ref.unlink(missing_ok=True)
        native.unlink(missing_ok=True)
        cmd = [sys.executable, "-I", str(HERE / "gambatte_dsy.py"), str(args.lib), str(args.rom), str(args.bios),
               str(args.movie), str(ref), "--frames", str(args.frames)]
        if args.bizhawk_23:
            cmd.append("--bizhawk-23")
        if args.bizhawk_232:
            cmd.append("--bizhawk-232")
        if args.digest:
            cmd.append("--digest")
        cmd += ["--from-frame", str(args.from_frame)]
        print("reference:", " ".join(cmd), file=sys.stderr)
        refproc = subprocess.Popen(cmd, stderr=subprocess.DEVNULL)   # the 2.3 core logs DIV events on stderr
        env = dict(os.environ, DEEPEEL_TAS_CGB_BIOS=str(args.bios.resolve()), DEEPEEL_DSY_FRAMES=str(args.frames),
                   DEEPEEL_DSY_FROM=str(args.from_frame), DEEPEEL_DSY_DIGEST="1" if args.digest else "0")
        if "2.3.2" in emu:   # cycle RTC with the movie's divisor offset
            env["DEEPEEL_TAS_RTCOFF"] = str(rtc_offset(args.movie))
        cmd = [str(args.deepeel), "explore", str(args.rom), "--tas", str(args.movie), "--replay-only", "--jobs", "1",
               "--no-event-probes", "--no-pointer-scan", "--no-closure", "--max-total", "0", "--dsy", str(native)]
        print("native   :", " ".join(cmd), file=sys.stderr)
        subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if refproc.wait():
            sys.exit("reference producer failed")

    total, first, count, detail = compare(native, ref, args.boot, args.show, set(args.ignore))
    print(f"compared {total} frame(s), boot noise below frame {args.boot} ignored")
    if not first:
        print("IDENTICAL: no block diverges")
        return 0
    print("first divergence per block (frame, differing frames in total):")
    for name, frame in sorted(first.items(), key=lambda kv: kv[1]):
        print(f"  {name:12s} frame {frame:7d}  ({count[name]} differing frame(s))")
    for name, frame in sorted(first.items(), key=lambda kv: kv[1]):
        n, x, y, prev, idx = detail[name]
        c, text = show(name, x, y, args.show, idx)
        print(f"\n[{name}] first at frame {n}: {c} byte(s) differ   native/reference: {text}")
        if name == "regs":
            fa, fb = struct.unpack("<HHBBHHH", x), struct.unpack("<HHBBHHH", y)
            print("  native   " + " ".join(f"{r}={v:04x}" for r, v in zip(REGS, fa)))
            print("  reference" + " ".join(f" {r}={v:04x}" for r, v in zip(REGS, fb)))
        if prev and prev[1].get("regs"):
            fx = struct.unpack("<HHBBHHH", prev[1]["regs"][0])
            print(f"  previous frame {prev[0]} regs " + " ".join(f"{r}={v:04x}" for r, v in zip(REGS, fx)))
    return 1


if __name__ == "__main__":
    sys.exit(main())

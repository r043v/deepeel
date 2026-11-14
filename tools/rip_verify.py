#!/usr/bin/env python3
"""Apply the local rip library to every local dump: byte-verify each rip
against each dump of its platform and write the `.verified` sidecars the
music-index build ingests.

Where BAB's rows carry the mapping (crc + copy tables), this re-derives it —
the rip data block IS the engine's bytes, so a fragment vote relocates it in
the dump exactly like dp_gbs_verify does on GB: 16-byte fragments sampled
across each bank segment, candidate deltas from the first hits, accepted when
>= --min-frag of them agree on one delta.  init/play are then projected
through the matched segments into ROM space (dp_rom_index: bank * 0x4000 +
(addr & 0x3FFF) — the NES/PCE explorers read physical offsets the same way).

Evidence, never trust: only a strong vote writes a sidecar; refusals are
counted per rip and printed.  A rip whose engine lives in RAM (MSX tapes) or
whose dump is absent simply does not match — nothing is forced.

Formats and dump conventions:
  gbs -> GB (16K banks), nsf/nsfe -> NES (4K NSF banks, iNES header stripped),
  hes -> PCE (8K MPR pages, copier header stripped), kss -> MSX (linear block,
  tape rips will not match).  The sidecar CRC is the raw dump file's CRC32 —
  the same key --music-index hashes at run time.  Already-verified pairs are
  skipped, so the tool just re-runs.

Resumable, read-only on the dumps; sidecars land next to the rips."""
import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from music_index import rip_parse          # noqa: E402  (header parsers, one per format)

FRAG = 16                                  # fragment size, like dp_gbs_verify
MIN_SEG = 512                              # a bank segment smaller than this is not evidence
# format -> (dump dirs under the ROM root, dump extensions, rip bank size)
PLATFORMS = {
    "gbs":  (["gb"],                     (".gb", ".gbc"), 0x4000),
    "nsf":  (["nes"],                    (".nes",),       0x1000),
    "nsfe": (["nes"],                    (".nes",),       0x1000),
    "hes":  (["pce"],                    (".pce",),       0x2000),
    "kss":  (["msx", "gg", "sms", "sg"], (".rom", ".mx1", ".gg", ".sms", ".sg"), 0x2000),
}
NSF_HDR = 128                              # NSF header size (NSFE handled chunk-wise)
HES_HDR = 0x20                             # HES header: vectors at 8, MPR banks at 0x10


def load_dumps(root, dirs, exts):
    """{crc32: (path, data)} for every dump: data is the ROM image the rip must
    match (iNES/copier headers stripped), crc is the RAW file's CRC32 — the
    key --music-index hashes."""
    out = {}
    for d in dirs:
        for p in sorted((root / d).glob("*")) if (root / d).is_dir() else []:
            if p.suffix.lower() not in exts or not p.is_file():
                continue
            raw = p.read_bytes()
            data = raw
            if p.suffix.lower() == ".nes" and raw[:4] == b"NES\x1a":
                prg = raw[4] * 0x4000
                off = 16 + (512 if raw[6] & 0x04 else 0)   # header + trainer
                data = raw[off:off + prg]
            elif p.suffix.lower() == ".pce" and len(raw) & 0x1FFF == 512:
                data = raw[512:]                           # copier header
            out[zlib.crc32(raw) & 0xFFFFFFFF] = (p, data)
    return out


def vote(data, rom, nfrag=12, min_frag=8):
    """Dominant delta of `data` inside `rom` by fragment vote: (delta, hits)
    when >= min_frag fragments agree, else (None, best_hits)."""
    if len(data) < FRAG * 4 or len(rom) < len(data):
        return None, 0
    offs = [i * (len(data) - FRAG) // (nfrag - 1) for i in range(nfrag)]
    cands = []
    for o in offs:                                  # candidate deltas from the first hits
        at = rom.find(data[o:o + FRAG])
        for _ in range(4):
            if at < 0:
                break
            cands.append(at - o)
            at = rom.find(data[o:o + FRAG], at + 1)
    best, hits = None, 0
    for d in set(cands):
        if d < 0 or d + len(data) > len(rom):
            continue
        n = sum(1 for o in offs if rom[d + o:d + o + FRAG] == data[o:o + FRAG])
        if n > hits:
            best, hits = d, n
    if best is not None and hits >= min_frag and hits * 4 >= nfrag * 3:
        return best, hits
    return None, hits


def rip_data(g, fmt, raw):
    """(data block, load address or None) of one rip; None when unknown."""
    if fmt == "gbs":
        return raw[112:], g["load"]
    if fmt == "nsf":
        return raw[NSF_HDR:], g["load"]
    if fmt == "nsfe":
        d, p = b"", 4
        while p + 8 <= len(raw):
            ln, name = struct.unpack_from("<I4s", raw, p)
            if name == b"DATA":
                d += raw[p + 8:p + 8 + ln]
            p += 8 + ln
        return (d, g.get("load")) if d else (None, None)
    if fmt == "hes":
        return raw[HES_HDR:], None
    if fmt == "kss":
        return raw[0x20:] if len(raw) > 0x20 else None, g.get("load")
    return None, None


def hes_foff(raw, addr, data_size):
    """HES: address -> data offset through the header's MPR page mapping
    (8 bank bytes at 0x10, one per 8K file bank)."""
    if len(raw) < HES_HDR:
        return None
    page = (addr >> 13) & 7
    for i in range(8):
        if raw[HES_HDR - 16 + i] == page:
            foff = i * 0x2000 + (addr & 0x1FFF)
            return foff if foff < data_size else None
    return None


def verify_rip(g, fmt, raw, dumps, opts):
    """Match one rip against every dump; returns the sidecar lines written."""
    data, load = rip_data(g, fmt, raw)
    if data is None or (load is None and fmt != "hes") or len(data) < opts.min_data:
        return [], f"rip unusable (data {len(data) if data else 0}, load {load})"
    bsize = PLATFORMS[fmt][2]
    lines, refusals = [], 0
    for crc, (path, rom) in dumps.items():
        # segments: rip banks of the platform size; each votes its own delta
        chunks = []
        for s in range(0, len(data), bsize):
            seg = data[s:s + bsize]
            if len(seg) < MIN_SEG:
                continue
            d, hits = vote(seg, rom)
            if d is None:
                continue
            chunks.append([d, s, len(seg)])
        if not chunks:
            refusals += 1
            continue
        def proj(addr):
            if addr is None:
                return None
            if fmt == "hes":                       # HES: through the MPR pages
                foff = hes_foff(raw, addr, len(data))
            else:
                foff = addr - load
            if foff is None or foff < 0:
                return None
            for r, dest, ln in chunks:
                if dest <= foff < dest + ln:
                    r = r + (foff - dest)
                    # an entry point is only recorded when its own bytes are
                    # verbatim in the dump — init==load heads are often the
                    # ripper's stub, which exists nowhere in the ROM (the SML
                    # lesson: 23/24 fragments verbatim, the head is not)
                    n = min(FRAG, len(data) - foff)
                    if rom[r:r + n] != data[foff:foff + n]:
                        return None
                    return r
            return None
        irom, prom = proj(g.get("init")), proj(g.get("play"))
        if irom is None and prom is None:
            refusals += 1
            continue
        ch = ",".join(f"{r}:{d}:{l}" for r, d, l in chunks)
        ln = f"dump crc={crc:08x} load={load} data_size={len(data)} chunks={ch}"
        if irom is not None:
            ln += f" init_rom={irom}"
        if prom is not None:
            ln += f" play_rom={prom}"
        ln += f" rom={path.name}"
        lines.append(ln)
    return lines, f"{refusals} dump(s) refused" if refusals else ""


def read_crcs(vpath: Path):
    out = set()
    if vpath.exists():
        for ln in vpath.read_text().splitlines():
            if ln.startswith("dump crc="):
                try:
                    out.add(int(ln[9:17], 16))
                except ValueError:
                    pass
    return out


def translate_rows(db, root, outdir, opts):
    """Re-locate every database row's engine block in the OTHER local dumps of
    its platform: the row's chunks say where the block lives in ITS dump (crc1);
    that dump's bytes are re-read, voted into every sibling dump, and a
    translation is written when the chunk shifts agree on one delta — the
    engine sits contiguously in a region, so its shift is constant.  This is
    how a row recorded for the Japanese dump seeds the European one (the
    engines are shared across regions), with the offset delta measured, never
    guessed.  One sidecar line per translated dump; the music-index build
    clones the row for it."""
    import sqlite3
    outdir.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(db)
    con.row_factory = sqlite3.Row
    rows = con.execute(
        "SELECT id, source, platform, crc1, chunks, init_rom, play_rom FROM entry "
        "WHERE crc1 IS NOT NULL AND chunks IS NOT NULL AND chunks != '[]' AND chunks != 'null'").fetchall()
    plat_of = {"gb": ["gbs"], "gg": ["kss", "sgc"], "sms": ["kss", "sgc"], "sg": ["kss", "sgc"],
               "mk3": ["kss", "sgc"], "msx": ["kss"], "nes": ["nsf", "nsfe"], "pce": ["hes"]}
    dump_cache = {}
    n_tr = 0
    for r in rows:
        plat = (r["platform"] or "").lower()
        fmts = plat_of.get(plat)
        if not fmts:
            continue
        dirs, exts, _ = PLATFORMS[fmts[0]]
        if dirs[0] not in dump_cache:
            dump_cache[dirs[0]] = load_dumps(root, dirs, exts)
        dumps = dump_cache[dirs[0]]
        src = dumps.get(int(r["crc1"], 16))
        if not src:
            continue
        ch = [c for c in json.loads(r["chunks"]) if len(c) == 3]
        if not ch:
            continue
        vpath = outdir / f"{r['id']}.verified"
        done = read_crcs(vpath)
        for crc, (path, rom) in dumps.items():
            if crc == int(r["crc1"], 16) or crc in done:
                continue
            tch, deltas = [], set()
            for crom, dest, ln in ch:
                if crom + ln > len(src[1]):
                    continue
                d, _ = vote(src[1][crom:crom + ln], rom)
                if d is None:
                    continue
                tch.append([crom + d, dest, ln])
                deltas.add(d)
            if not tch or len(deltas) > 1:      # the shift must be one constant
                continue
            shift = next(iter(deltas))
            line = (f"dump crc={crc:08x} chunks="
                    + ",".join(f"{a}:{b}:{c2}" for a, b, c2 in tch))
            for k, what in (("init_rom", r["init_rom"]), ("play_rom", r["play_rom"])):
                if what is None:
                    continue
                inside = [c for c in ch if c[0] <= what < c[0] + c[2]]
                if not inside:
                    continue
                foff = inside[0][1] + (what - inside[0][0])
                tgt = next((a + (foff - b) for a, b, _ in tch if b == inside[0][1]), None)
                if tgt is None:
                    continue
                # the entry point's own bytes must have travelled verbatim
                n = min(FRAG, len(src[1]) - what)
                if src[1][what:what + n] == rom[tgt:tgt + n]:
                    line += f" {k}={tgt}"
            line += f" src={r['crc1']} rom={path.name}"
            n_tr += 1
            if not opts.dry_run:
                with vpath.open("a") as f:
                    f.write(line + "\n")
            print(f"row {r['id']} ({r['source']}): {path.name} shift {shift:+#x}", flush=True)
    return n_tr, len(rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rips", action="append", default=["output/gbs", "output/joshw"])
    ap.add_argument("--roms", default=str(Path.home() / "dev/m0boy/roms"))
    ap.add_argument("--min-data", type=int, default=0x800, help="smallest rip block worth matching")
    ap.add_argument("--formats", default="gbs,nsf,nsfe,hes,kss")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--translate", action="store_true",
                    help="re-locate every database row's engine block in the other local "
                         "dumps of its platform (region sharing), not the rip files")
    ap.add_argument("--db", default="output/music_index/music_index.sqlite3")
    ap.add_argument("--translate-dir", default="output/bab/translate")
    args = ap.parse_args()
    if args.translate:
        n_tr, n_rows = translate_rows(args.db, Path(args.roms), Path(args.translate_dir), args)
        print(f"translate: {n_tr} dump(s) translated from {n_rows} row(s)")
        return

    root = Path(args.roms)
    dump_cache = {}
    n_ok = n_skip = n_fail = 0
    for rd in args.rips:
        for rp in sorted(Path(rd).rglob("*")):
            if not rp.is_file():
                continue
            fmt = rp.suffix.lower()[1:]
            if fmt not in PLATFORMS or fmt not in args.formats.split(","):
                continue
            g = rip_parse(rp)
            if not g or g.get("init") is None and g.get("play") is None:
                n_fail += 1
                continue
            dirs, exts, _ = PLATFORMS[fmt]
            key = (dirs[0], tuple(exts))
            if key not in dump_cache:
                dump_cache[key] = load_dumps(root, dirs, exts)
            dumps = dump_cache[key]
            if not dumps:
                continue
            vpath = Path(str(rp) + ".verified")
            done = read_crcs(vpath)
            fresh = [c for c in dumps if c not in done]
            if not fresh:
                n_skip += 1
                continue
            raw = rp.read_bytes()
            lines, why = verify_rip(g, fmt, raw, {c: dumps[c] for c in fresh}, args)
            if lines and not args.dry_run:
                with vpath.open("a") as f:
                    f.write("\n".join(lines) + "\n")
            if lines:
                n_ok += 1
                print(f"{rp.name}: {len(lines)} dump(s) verified", flush=True)
            else:
                n_fail += 1
                if "--verbose" in sys.argv:
                    print(f"{rp.name}: no match ({why})", flush=True)
    print(f"rip_verify: {n_ok} verified, {n_skip} already done, {n_fail} without a match")


if __name__ == "__main__":
    main()

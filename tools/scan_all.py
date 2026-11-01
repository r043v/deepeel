#!/usr/bin/env python3
# scan_all.py: multi-ROM campaign driver for deepeel.
#
#   scan   run the generic recipe on every ROM of a directory, one bounded
#          run each (graceful --time-limit wall, memory cap, TAS movies mined
#          from local dirs by title match); outputs land in out/<slug>/
#   digest aggregate the per-run summary JSONs into REPORT.md: coverage
#          table, residual histograms, saturation counters — the raw material
#          of the "what still blocks 100%" analysis
#
# Every run is an observation of the tool (concrete machine, zeroed RAM),
# not a hardware measurement.
import argparse
import json
import os
import re
import subprocess
import sys
import time

# real movie containers only: output/tas/*.log are this tool's own run logs,
# not input logs — the C-side --tas-dir still accepts .log on purpose
MOVIE_EXTS = (".vbm", ".vmv", ".bk2", ".gbm", ".fm2")
ROM_EXTS = (".gb", ".gbc")


def norm(s):
    return re.sub(r"[^a-z0-9]", "", (s or "").lower())


def longest_common_substring(a, b):
    if not a or not b:
        return 0
    best = 0
    for i in range(len(a)):
        for j in range(len(b)):
            l = 0
            while i + l < len(a) and j + l < len(b) and a[i + l] == b[j + l]:
                l += 1
            if l > best:
                best = l
    return best


def rom_title(path):
    with open(path, "rb") as f:
        f.seek(0x134)
        raw = f.read(0x10)
    return raw.split(b"\0", 1)[0].decode("ascii", "replace")


def rom_banks(path):
    return os.path.getsize(path) // 0x4000


def slugify(title):
    s = re.sub(r"[^A-Za-z0-9]+", "-", title).strip("-").lower()
    return s[:40] or "rom"


def score_against(m, t):
    """0..100 for one normalized candidate name."""
    idx = m.find(t)
    if idx >= 0:
        end = idx + len(t)
        if end == len(m) or not m[end].isalnum():
            return 100   # clean containment
        if m[end].isdigit():
            return 5     # sequel marker: "supermarioland2" is NOT the first game
        return 80        # title followed by letters ("zeldala")
    return min(longest_common_substring(m, t), 12)


def match_score(movie, rom_title, rom_file=""):
    """best score of a movie filename against a ROM's cart title and filename.

    The filename also provides its word-initial acronym ("Super Mario Land"
    -> "sml") so community shorthand names match too.
    """
    m = norm(movie)
    best = score_against(m, norm(rom_title))
    if rom_file:
        base = re.sub(r"\.[^.]+$", "", rom_file)  # drop the extension
        base = re.sub(r"\(.*?\)", "", base)       # drop "(France)" etc.
        best = max(best, score_against(m, norm(base)))
        words = [w for w in re.split(r"[^0-9a-zA-Z]+", base) if len(w) >= 2]
        if len(words) >= 2:
            best = max(best, score_against(m, "".join(w[0] for w in words).lower()))
    return best


def assign_movies(tas_dirs, roms):
    """give every movie file to the ROM it scores best (min score 8).

    roms: list of dicts with 'slug', 'title' and 'file'.  Returns {slug: [paths]}.
    Exclusive assignment: each movie goes to exactly one ROM (or none).
    """
    files = []
    for root_dir in tas_dirs:
        for dirpath, _dirs, names in os.walk(root_dir):
            for name in names:
                if name.lower().endswith(MOVIE_EXTS):
                    files.append(os.path.join(dirpath, name))
    assign = {r["slug"]: [] for r in roms}
    for path in sorted(files):
        best, best_score = None, 0
        for r in roms:
            s = match_score(os.path.basename(path), r["title"], r.get("file", ""))
            if s > best_score:
                best, best_score = r["slug"], s
        if best is not None and best_score >= 8:
            assign[best].append(path)
    return {k: v[:8] for k, v in assign.items()}


def max_total_for(banks):
    if banks <= 16:
        return 100_000_000
    if banks <= 32:
        return 200_000_000
    return 300_000_000


def cmd_scan(args):
    roms = sorted(
        f for f in os.listdir(args.roms)
        if f.lower().endswith(ROM_EXTS) and os.path.isfile(os.path.join(args.roms, f))
    )
    if not roms:
        sys.exit(f"scan_all: no .gb/.gbc ROM in {args.roms}")
    entries = [{"path": os.path.join(args.roms, n), "title": rom_title(os.path.join(args.roms, n)),
                "slug": slugify(rom_title(os.path.join(args.roms, n))), "file": n} for n in roms]
    movies_by_slug = assign_movies(args.tas_dirs, entries)
    os.makedirs(args.out, exist_ok=True)
    binary = os.path.abspath(args.binary)

    for e in entries:
        path, title, slug = e["path"], e["title"], e["slug"]
        if args.only and args.only != slug:
            continue
        out_dir = os.path.join(args.out, slug)
        os.makedirs(out_dir, exist_ok=True)
        summary_path = os.path.join(out_dir, f"{slug}.summary.json")
        time_limit = 0 if args.full else args.time_limit

        cmd = [
            binary, "explore", path,
            "--play",
            "--queue", "4096", "--queue-max", str(args.queue_max),
            "--mem-cap", str(args.mem_cap),
            "--spool-cap-mb", str(args.spool_cap_mb),
            "--jobs", str(args.jobs),
            "--covered-keep", "8", "--seed-banks",
            "--closure-rounds", "12",
            "--max-total", str(max_total_for(rom_banks(path))),
            "--time-limit", str(time_limit),
            "--summary", summary_path,
            "--text", os.path.join(out_dir, f"{slug}.txt"),
            "--map", os.path.join(out_dir, f"{slug}.map.png"),
            "--state-dir", os.path.join(out_dir, "state"),
        ]
        movies = movies_by_slug.get(slug, [])
        for m in movies:
            cmd += ["--tas", m]
        if movies and not args.full:
            # bounded pass: only the movies play.  A full pass keeps the random
            # lanes: resumed movies finish at once (their snapshots are already
            # in the pool) and the new routes come from the random play.
            cmd += ["--tas-solo"]
        if args.resume and os.path.exists(os.path.join(out_dir, "state", "pool.bin")):
            cmd += ["--resume"]

        log_path = os.path.join(out_dir, f"{slug}.log")
        print(f"=== {slug}: {title!r} ({rom_banks(path)} banks), "
              f"{len(movies)} TAS movie(s), time-limit {time_limit or 'none'}")
        t0 = time.time()
        hard_kill = (time_limit + 1800) if time_limit else None
        with open(log_path, "w") as log:
            proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT)
            try:
                proc.wait(timeout=hard_kill)
            except subprocess.TimeoutExpired:
                print(f"scan_all: {slug}: hard kill (deepeel missed its own wall)")
                proc.kill()
                proc.wait()
        print(f"    done in {time.time() - t0:.0f}s (rc={proc.returncode})")

        ok = False
        if os.path.exists(summary_path):
            try:
                s = json.load(open(summary_path))
                ok = True
                print(f"    known {s['map']['known_pct']:.1f}%  "
                      f"unknown {s['map']['unknown_pct']:.3f}%  "
                      f"({s['map']['residual_ranges']} residual ranges)")
            except (json.JSONDecodeError, KeyError):
                pass
        if not ok:
            print("    NO SUMMARY — check the log")


def hist_buckets(ranges):
    buckets = {"1-3": 0, "4-7": 0, "8-15": 0, "16-63": 0, "64-255": 0, "256+": 0}
    for r in ranges:
        n = r["len"]
        key = ("1-3" if n <= 3 else "4-7" if n <= 7 else "8-15" if n <= 15
               else "16-63" if n <= 63 else "64-255" if n <= 255 else "256+")
        buckets[key] += 1
    return buckets


def load_bank_map(map_path):
    """bank -> (starts, ends, classes), addresses converted back to
    bank-relative (the .map stores CPU-view addresses)."""
    import bisect
    perbank = {}
    with open(map_path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            b, s, e, cls = line.split()
            b = int(b)
            win = 0x4000 if b else 0
            B = perbank.setdefault(b, ([], [], []))
            B[0].append(int(s, 16) - win)
            B[1].append(int(e, 16) - win)
            B[2].append(cls)
    return perbank, bisect


def residual_anatomy(perbank, bisect, residual):
    """classify every residual range by its classified neighbours — the
    cross-ROM mechanism map: which engine rule would take which bytes."""
    def cls_at(bb, aa):
        starts, ends, cls = perbank[bb]
        i = bisect.bisect_right(starts, aa) - 1
        if i >= 0 and starts[i] <= aa <= ends[i]:
            return cls[i]
        return "EOF"

    def kind(c):
        if "code" in c:
            return "c"
        if c.startswith("data") or c == "table":
            return "d"
        if "pad" in c:
            return "p"
        return "?"

    bytes_by, cnt_by = {}, {}
    for r in residual:
        b, lo, hi, ln = r["bank"], r["lo"], r["hi"], r["len"]
        if b not in perbank:
            k = "hors carte"
        else:
            bef = cls_at(b, lo - 1) if lo > 0 else "bankedge"
            aft = cls_at(b, hi + 1) if hi < 0x3FFF else "bankedge"
            kb, ka = kind(bef), kind(aft)
            if kb == "c" and ka == "c":
                k = "code|code (jamais execute, > closure)"
            elif kb == "d" and ka == "d":
                k = "data|data (trou dans un bloc lu)"
            elif {kb, ka} == {"c", "d"}:
                k = "code|data (bord mixte, jamais infere)"
            elif "p" in (kb, ka):
                k = "bord padding (frange de tuiles)"
            elif "bankedge" in (bef, aft):
                k = "bord de banque"
            else:
                k = "voisins non classes"
        bytes_by[k] = bytes_by.get(k, 0) + ln
        cnt_by[k] = cnt_by.get(k, 0) + 1
    return bytes_by, cnt_by


def cmd_digest(args):
    rows = []
    for slug in sorted(os.listdir(args.out)):
        sp = os.path.join(args.out, slug, f"{slug}.summary.json")
        if not os.path.isfile(sp):
            continue
        try:
            s = json.load(open(sp))
        except json.JSONDecodeError:
            continue
        s["_slug"] = slug
        rows.append(s)
    if not rows:
        sys.exit("scan_all: no summary found — run the scan first")

    lines = ["# Rapport de campagne multi-ROMs (deepeel)",
             "",
             "Observations de l'outil (machine concrète, RAM à zéro) —",
             "pas des mesures de matériel.", ""]

    # coverage table
    lines += ["## Couverture", "",
              "| ROM | banques | connu | inconnu | inféré | résidu (o / plages) | pas | chemins |",
              "|---|---|---|---|---|---|---|---|"]
    for s in rows:
        m = s["map"]
        lines.append(
            f"| {s['_slug']} | {s['rom']['banks']} | {m['known_pct']:.2f}% | "
            f"{m['unknown_pct']:.3f}% | {m['inferred_pct']:.2f}% | "
            f"{m['residual_bytes']} / {m['residual_ranges']} | "
            f"{s['counters']['steps']:,} | {s['counters']['paths']:,} |")
    lines.append("")

    # saturation / budget table
    lines += ["## Murs et saturations", "",
              "| ROM | file: pertes / croissances | fourches bouchées | murs de pas |"
              " arbre ouvert | closure: refus / banques / indirect | exec hors ROM |",
              "|---|---|---|---|---|---|---|"]
    for s in rows:
        c, cl, t = s["counters"], s["closure"], s["tree"]
        lines.append(
            f"| {s['_slug']} | {c['queue_overflows']} / {c['queue_growths']} | "
            f"{c['forks_capped']} | {c['budget_steps']} | {t['open']} | "
            f"{cl['seeds_refused']} / {cl['bank_seeds']} / {cl['indirect_seeds']} | "
            f"{c['exec_outside']:,} |")
    lines.append("")

    # residual histogram + largest ranges
    lines += ["## Anatomie du résidu (octets jamais observés ni inférés)", ""]
    for s in rows:
        m = s["map"]
        if not m["residual_ranges"]:
            lines.append(f"- **{s['_slug']}** : résidu nul.")
            continue
        h = hist_buckets(m["residual"])
        total = m["residual_bytes"]
        parts = ", ".join(f"{k}: {v}" for k, v in h.items() if v)
        lines.append(f"- **{s['_slug']}** : {total} o en {m['residual_ranges']} plages ({parts}).")
        top = sorted(m["residual"], key=lambda r: -r["len"])[:8]
        fmt = ", ".join(f"{r['bank']}:{0x4000 + r['lo']:04x}-{0x4000 + r['hi']:04x}"
                        if r["bank"] else
                        f"{r['bank']}:{r['lo']:04x}-{r['hi']:04x}" for r in top)
        lines.append(f"  plus grandes : {fmt}")
    lines.append("")

    # residual by neighbour context — which engine rule would unlock which bytes
    lines += ["## Résidu par contexte de bord (quel mécanisme débloque quoi)", "",
              "| ROM | code\\|code jamais exécuté | code\\|data bord mixte | bord padding | data\\|data | autre |",
              "|---|---|---|---|---|---|"]
    for s in rows:
        map_path = os.path.join(args.out, s["_slug"], f"{s['_slug']}.map")
        if not os.path.isfile(map_path):
            lines.append(f"| {s['_slug']} | (`.map` absent — extraire avec `deepeel mapx`) | | | | |")
            continue
        perbank, bisect = load_bank_map(map_path)
        by, cnt = residual_anatomy(perbank, bisect, s["map"]["residual"])

        def g(prefix):
            return sum(v for k, v in by.items() if k.startswith(prefix))

        other = sum(v for k, v in by.items()
                    if not k.startswith(("code|", "code |", "bord padding", "data|data")))
        lines.append(f"| {s['_slug']} | {g('code|code')} | {g('code|data')} | {g('bord padding')} | "
                     f"{g('data|data')} | {other} |")
    lines.append("")

    # mechanical blocker ranking, from the counters
    lines += ["## Mécanismes bloquants détectés", ""]
    for s in rows:
        c, cl = s["counters"], s["closure"]
        notes = []
        if c["queue_overflows"]:
            notes.append(f"**{c['queue_overflows']} chemins entiers perdus** (file saturée, "
                         f"{c['queue_growths']} croissances déjà payées — monter --queue-max)")
        if c["budget_steps"]:
            notes.append(f"mur de pas touché {c['budget_steps']}× (la recherche s'est arrêtée avant l'épuisement)")
        if c["forks_capped"]:
            notes.append(f"{c['forks_capped']} fourches bouchées par le cap par site")
        if cl["unresolved_bank"]:
            notes.append(f"{cl['unresolved_bank']} arêtes banque ambiguës non tranchées")
        if c["exec_outside"]:
            notes.append(f"{c['exec_outside']:,} pas d'exécution hors ROM (dispatch calculé — "
                         "les cibles VRAM/WRAM ne prouvent rien sur la carte)")
        if s["play"]["stop_reason"] == "time budget reached":
            notes.append("arrêté par le mur horloge (le résidu peut encore baisser avec plus de temps)")
        lines.append(f"- **{s['_slug']}** : " + (" ; ".join(notes) if notes else "aucun mur visible — "
                     "le résidu est intouchable par les mécanismes actuels."))
    lines.append("")

    report = os.path.join(args.out, "REPORT.md")
    with open(report, "w") as f:
        f.write("\n".join(lines))
    print(f"scan_all: {report} written ({len(rows)} runs)")
    print("\n".join(lines[:14]))


def main():
    ap = argparse.ArgumentParser(description="deepeel multi-ROM campaign driver")
    sub = ap.add_subparsers(dest="mode", required=True)

    scan = sub.add_parser("scan", help="run the generic recipe on every ROM")
    scan.add_argument("--roms", default=os.path.expanduser("~/dev/m0boy/roms/gb"))
    scan.add_argument("--out", default="output/batch")
    scan.add_argument("--tas-dirs", nargs="*", default=["output/tas"])
    scan.add_argument("--binary", default="build/deepeel")
    scan.add_argument("--time-limit", type=int, default=1800, help="play wall seconds per ROM (0 = none)")
    scan.add_argument("--mem-cap", type=int, default=3500, help="RSS cap in MiB")
    scan.add_argument("--queue-max", type=int, default=32768, help="worklist growth ceiling")
    scan.add_argument("--spool-cap-mb", type=int, default=3000, help="disk spool cap in MiB")
    scan.add_argument("--jobs", type=int, default=min(16, os.cpu_count() or 1),
                      help="parallel drain workers")
    scan.add_argument("--only", help="single slug")
    scan.add_argument("--full", action="store_true", help="no time limit (the 'à fond' pass)")
    scan.add_argument("--resume", action="store_true", help="resume from the saved pool if any")

    dig = sub.add_parser("digest", help="aggregate the summaries into REPORT.md")
    dig.add_argument("--out", default="output/batch")

    args = ap.parse_args()
    if args.mode == "scan":
        cmd_scan(args)
    else:
        cmd_digest(args)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# compare_platforms.py: cross-platform comparative table (axis 4).
#
#   compare_platforms.py results/... [results/... ...] [--out FILE]
#
# Walks the given campaign directories, reads every summary.json and renders
# a comparative markdown table: one row per ROM, grouped by platform, plus a
# per-platform aggregate line.  The summary schemas differ per platform
# (gb/nes/md/sega) so every field is best-effort: a missing field prints as
# "—".  Numbers are observations of the tool (concrete machine, zeroed RAM),
# never hardware measurements; budgets differ between campaigns, so the
# table compares what was run, not the platforms' intrinsic difficulty.
import argparse
import json
import os
import sys

PLATFORMS = ["gb", "nes", "md", "sg", "sms", "gg", "msx", "pce"]


def platform_of(path, summary):
    for part in path.replace("\\", "/").split("/"):
        for p in PLATFORMS:
            if part == p or part.startswith(p + "-"):
                return p
    return summary.get("platform") or summary.get("system") or "?"


def pick(summary, *keys):
    for k in keys:
        if isinstance(summary, dict) and k in summary:
            return summary[k]
    return None


def one(summary, section, key):
    v = summary.get(section)
    return v.get(key) if isinstance(v, dict) else None


def row_of(path, summary):
    plat = platform_of(path, summary)
    rom = summary.get("rom") or {}
    title = rom.get("title") or os.path.basename(os.path.dirname(path))
    known = pick(summary, "known_pct")
    if known is None:
        known = one(summary, "map", "known_pct")
    size = rom.get("size") or rom.get("bytes") or pick(summary, "prg_size", "rom_size")
    c = summary.get("counters") or {}
    return {
        "platform": plat,
        "rom": title,
        "size": size,
        "known": known,
        "inferred": one(summary, "map", "inferred_pct") if "inferred_pct" in (summary.get("map") or {}) else pick(summary, "inferred_pct"),
        "steps": c.get("steps"),
        "paths": c.get("paths"),
        "forks": c.get("forks") or c.get("forks_branch"),
        "exec": c.get("exec_bytes"),
    }


def fmt(v, pct=False):
    if v is None:
        return "—"
    if pct:
        return f"{v:.1f} %"
    return f"{v:,}".replace(",", " ")


def main():
    ap = argparse.ArgumentParser(description="cross-platform comparative table from summary.json files")
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--out", default=None, help="write the markdown here (default: stdout)")
    args = ap.parse_args()

    rows = []
    for d in args.dirs:
        for root, _, files in os.walk(d):
            if "summary.json" in files:
                try:
                    s = json.load(open(os.path.join(root, "summary.json")))
                except (OSError, ValueError) as e:
                    print(f"compare_platforms: {root}: {e}", file=sys.stderr)
                    continue
                rows.append(row_of(os.path.join(root, "summary.json"), s))

    if not rows:
        sys.exit("compare_platforms: no summary.json found")

    out = sys.stdout if not args.out else open(args.out, "w")
    out.write("# Tableau comparatif des plateformes\n\n")
    out.write("Observations de l'outil (machine concrète, RAM à zéro) — pas des mesures de matériel. "
              "Les budgets diffèrent d'une campagne à l'autre : le tableau compare ce qui a été "
              "exécuté, pas la difficulté intrinsèque des plateformes.\n\n")
    out.write("Sources : " + ", ".join(f"`{d}/`" for d in args.dirs) + ".  ")
    out.write("Généré par `tools/compare_platforms.py`. Le PCE n'expose pas encore "
              "`--summary` (tableau texte seul) : ligne à ajouter quand il l'aura.\n")
    cur = None
    for r in sorted(rows, key=lambda r: (PLATFORMS.index(r["platform"]) if r["platform"] in PLATFORMS else 99, r["rom"])):
        if r["platform"] != cur:
            cur = r["platform"]
            out.write(f"\n## {cur}\n\n")
            out.write("| ROM | taille | connu | inféré | pas | courses | fourches | octets exécutés |\n")
            out.write("|---|---|---|---|---|---|---|---|\n")
        out.write(f"| {r['rom']} | {fmt(r['size'])} | {fmt(r['known'], True)} | {fmt(r['inferred'], True)} | "
                  f"{fmt(r['steps'])} | {fmt(r['paths'])} | {fmt(r['forks'])} | {fmt(r['exec'])} |\n")
    out.write("\n## Agrégat par plateforme\n\n")
    out.write("| plateforme | ROMs | connu moyen |\n|---|---|---|\n")
    for p in PLATFORMS:
        rs = [r for r in rows if r["platform"] == p and r["known"] is not None]
        if rs:
            out.write(f"| {p} | {len(rs)} | {sum(r['known'] for r in rs) / len(rs):.1f} % |\n")
    if not args.out:
        out.flush()


if __name__ == "__main__":
    main()

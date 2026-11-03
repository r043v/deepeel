#!/usr/bin/env python3
# audit_matrix.py: J13 audit of docs/COUVERTURE.fr.md section 2.1. Parses the
# synthesis matrix, checks it is well-formed (7 platform cells per row, known
# symbols only), tallies done/partial/missing per platform and lists the open
# cells (the work left toward the norm "every line is done everywhere").
#   usage: audit_matrix.py COUVERTURE.fr.md [OUT.md]
# The marks are the document's own claims; this script audits their form and
# counts, it does not re-measure the tool.
import re, sys

text = open(sys.argv[1], encoding="utf-8").read()
start = text.index("### 2.1 Matrice de synthèse")
end = text.index("**Norme**", start)
rows = []
for line in text[start:end].splitlines():
    if not line.startswith("| ") or line.startswith("| #") or line.startswith("|---"):
        continue
    cells = [c.strip() for c in line.strip().strip("|").split("|")]
    rows.append(cells)

PLAT = ["GB", "SMS/GG", "MSX", "NES", "PCE", "MD"]
problems = []
tally = {p: {"done": 0, "partial": 0, "missing": 0, "na": 0} for p in PLAT}
open_cells = []
for r in rows:
    if len(r) != 8:
        problems.append(f"{r[0] if r else '?'}: {len(r)} cells, expected 8")
        continue
    rid, tech, vals = r[0], r[1], r[2:]
    for p, v in zip(PLAT, vals):
        if v.startswith("✅") or (v.count("✅") and "✗" not in v.split("/")[0] and not v.startswith("◐")):
            k = "done"
        elif v.startswith("◐"):
            k = "partial"
        elif v.startswith("✗"):
            k = "missing"
        elif v.startswith("—"):
            k = "na"
        else:
            problems.append(f"{rid} {p}: unknown mark {v!r}")
            continue
        tally[p][k] += 1
        if k in ("partial", "missing"):
            open_cells.append((rid, tech, p, v))

out = ["# Audit de la matrice §2.1", "",
       "Généré par `tools/audit_matrix.py` à partir de `docs/COUVERTURE.fr.md`. Les marques",
       "sont les affirmations du document ; ce script en audite la forme et les totaux, il ne",
       "remesure pas l'outil.", "",
       f"{len(rows)} lignes de technique, {len(PLAT)} plateformes.", "",
       "| Plateforme | ✅ fait | ◐ partiel | ✗ absent | — sans objet |", "|---|---|---|---|---|"]
for p in PLAT:
    t = tally[p]
    out.append(f"| {p} | {t['done']} | {t['partial']} | {t['missing']} | {t['na']} |")
out += ["", f"Cellules ouvertes (◐ ou ✗) : {len(open_cells)}.", "",
        "| # | Technique | Plateforme | État |", "|---|---|---|---|"]
for rid, tech, p, v in open_cells:
    out.append(f"| {rid} | {tech} | {p} | {v} |")
out += ["", "Problèmes de forme : " + (str(len(problems)) if problems else "aucun") + "."]
for pr in problems:
    out.append(f"- {pr}")
dest = sys.argv[2] if len(sys.argv) > 2 else None
if dest:
    open(dest, "w", encoding="utf-8").write("\n".join(out) + "\n")
print(f"{len(rows)} rows, {len(open_cells)} open cells, {len(problems)} problems")
sys.exit(1 if problems else 0)

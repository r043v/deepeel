#!/usr/bin/env python3
# campaign_fixtures.py: J13 multi-platform campaign over the generated test
# fixtures (no ROM from the repository, none committed): runs deepeel on every
# fixture of each platform and writes a table of tool observations.
#   usage: campaign_fixtures.py BUILD_DIR OUT.md
# Numbers are observations of the tool (concrete machine, zeroed RAM), not
# hardware measurements.
import glob, os, re, subprocess, sys, tempfile, time

build, out = sys.argv[1], sys.argv[2]
exe = os.path.join(build, "deepeel")
fx = os.path.join(build, "tests")

PLATFORMS = [  # name, glob, command builder
    ("Game Boy", "fix_*.gb", lambda rom, t, m: [exe, "explore", rom, "--text", t, "--map", m]),
    ("Mega Drive", "fix_md_*.md", lambda rom, t, m: [exe, "md", "explore", rom, "--text", t, "--map", m]),
    ("NES", "fix_nes_*.nes", lambda rom, t, m: [exe, "nes", "explore", rom, "--text", t, "--map", m]),
    ("PC Engine", "fix_pce_*.pce", lambda rom, t, m: [exe, "pce", "explore", rom, "--text", t, "--map", m]),
    ("Master System", "fix_*.sms", lambda rom, t, m: [exe, "z80", "explore", "sms", rom, "--text", t, "--map", m]),
]

def parse(text, mapfile):
    steps = None
    m = (re.search(r"exploration: (\d+) steps in (\d+) paths", text) or re.search(r"steps: (\d+) in (\d+) paths", text)
         or re.search(r"steps (\d+) paths (\d+)", text))
    if m:
        steps = int(m.group(1))
    known = None
    m = re.search(r"known +(\d+(?:\.\d+)?)%", text) or re.search(r"known \d+ \((\d+(?:\.\d+)?)%\)", text)
    mm = re.search(r"map \(tool observation, \d+ bytes\): known \d+ \((\d+(?:\.\d+)?)%\)", text)
    mn = re.search(r"coverage (\d+(?:\.\d+)?)%", text) or re.search(r"observed \d+ bytes \((\d+(?:\.\d+)?)%\)", text)
    if mm:
        known = mm.group(1)
    elif mn:
        known = mn.group(1) + " (observé)"
    elif os.path.exists(mapfile):
        mp = open(mapfile, errors="replace").read()
        m2 = re.search(r"#\s+known\s+(\d+(?:\.\d+)?)%", mp)
        if m2:
            known = m2.group(1)
    return steps, known

rows, fails = [], 0
with tempfile.TemporaryDirectory() as td:
    for name, pat, mk in PLATFORMS:
        for rom in sorted(glob.glob(os.path.join(fx, pat))):
            t, mp = os.path.join(td, "t.txt"), os.path.join(td, "m.map")
            for f in (t, mp):
                if os.path.exists(f):
                    os.remove(f)
            t0 = time.time()
            r = subprocess.run(mk(rom, t, mp), capture_output=True, text=True, timeout=300)
            dt = time.time() - t0
            text = open(t, errors="replace").read() if os.path.exists(t) else r.stdout
            steps, known = parse(text, mp)
            if r.returncode != 0:
                fails += 1
            rows.append((name, os.path.basename(rom), r.returncode, steps, known, dt))

with open(out, "w") as f:
    f.write("# Campagne multi-plateforme sur fixtures générées\n\n")
    f.write("Observations de l'outil (machine concrète, RAM à zéro) sur les fixtures\n")
    f.write("construites par `tests/build_fixtures.py` ; **pas** des mesures de matériel,\n")
    f.write("**pas** des ROMs réelles. Généré par `tools/campaign_fixtures.py`.\n\n")
    f.write("| Plateforme | Fixture | Code retour | Pas explorés | Octets connus (%) | Durée (s) |\n|---|---|---|---|---|---|\n")
    for name, rom, rc, steps, known, dt in rows:
        f.write(f"| {name} | `{rom}` | {rc} | {steps if steps is not None else 'n/d'} | {known if known is not None else 'n/d'} | {dt:.2f} |\n")
    f.write("\n")
    per = {}
    for r in rows:
        per.setdefault(r[0], []).append(r)
    f.write("## Synthèse par plateforme\n\n| Plateforme | Fixtures | Échecs | Pas (total) |\n|---|---|---|---|\n")
    for name, rs in per.items():
        f.write(f"| {name} | {len(rs)} | {sum(1 for r in rs if r[2])} | {sum(r[3] or 0 for r in rs)} |\n")
    f.write("\n`n/d` : valeur non exposée par le rapport texte de la plateforme.\n")
    f.write("Pour NES et PC Engine la colonne « octets connus » est la couverture observée\n")
    f.write("(octets touchés), pas une classification complète de la ROM.\n")
print(f"{len(rows)} runs, {fails} failures -> {out}")
sys.exit(1 if fails else 0)

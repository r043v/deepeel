#!/usr/bin/env python3
"""Fetch, extract and normalize BAB's local rip databases (mrchecker.web.fc2.com).

Downloads the "old game ROM research" music-ripper suite (2010-2022) plus the
legacy 2010 GBgbs.dat, extracts the TSV databases embedded in the .NET
executables and writes clean UTF-8 TSVs under output/ (git-ignored).

Everything lands in the destination directory (default output/bab/): the
archives, the extracted executables, the databases and a manifest. Nothing
from that directory may be redistributed (freeware, personal use, see the
generated README); the databases are only usable locally as a validation
oracle, facts must be re-derived by deepeel before entering the repo.

Requires: python3 stdlib, 7z (lha/zips with non-UTF-8 names), optionally
mono + mcs (full embedded-resource extraction; without it a byte-carving
fallback handles most databases, the NES ones need mono).
"""

import argparse
import csv
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile
import urllib.request
from pathlib import Path

SITE = "http://mrchecker.web.fc2.com"
# 2010 GBgbs V1.0: nesdev attachments need a logged-in session, use the
# Wayback Machine snapshot instead.
LEGACY_URL = ("https://web.archive.org/web/20211205011219if_/"
              "https://forums.nesdev.org/download/file.php?id=11571")
LEGACY_SHA256 = "544204abdbdc28f6579cb2a3b88cb9a8a0a07ec8fe6331107661495184dcf3d8"

# name, url, sha256 (None = not pinned), kind
MUSIC_TOOLS = [
    ("gb_ongaku_v20220606",   f"{SITE}/download/gb_ongaku_v20220606.zip",
     "48bd74cbfb5bac36fc88cd264097d8e9f624675343f315cc8755ccf2df2dc10a"),
    ("famion_v2_v20220606",   f"{SITE}/download/famion_v2_v20220606.zip",
     "d13d9539c2bf22565236376572d9802db51b61c769f05a0547120d450814d09f"),
    ("pce_ongaku_v20220606",  f"{SITE}/download/pce_ongaku_v20220606.zip",
     "54bf664cfce5c471515ea1db8f521c6e296c7bddbbfc9144a4b4c31b3ac48164"),
    ("msx_ongaku_v20201121",  f"{SITE}/download/msx_ongaku_v20201121.zip",
     "ecb5b292cc5d701e141ecc55d869e0463c649d7f822e3e49edf47c1db8e7d48d"),
    ("gg_ongaku_k20220611",   f"{SITE}/download/gg_ongaku_k20220611.zip",
     "f272081b51f8c855715dd1be95bbbeee734654eec191047e60901ba28c9684ef"),
    ("gg_ongaku_s20220611",   f"{SITE}/download/gg_ongaku_s20220611.zip",
     "57a0d977ab68119bb6f1dff4ca7329fc4e0538d07ffcb21e7897b2b8718d6989"),
    ("mk3_ongaku_k20220219",  f"{SITE}/download/mk3_ongaku_k20220219.zip",
     "f8592bdf5fc501da5de0e60553d83cb30efd97a69228d052da57a8a3f54e3d1a"),
    ("mk3_ongaku_s20220219",  f"{SITE}/download/mk3_ongaku_s20220219.zip",
     "018f701af295bac6ce746479da17377b34dbd8b62f5bb115d2ca853704e1482a"),
    ("scsg_ongaku_k20190914", f"{SITE}/download/scsg_ongaku_k20190914.zip",
     "12230a4bb7c4c73c204ba57f31ad8d12a28bc4b2eda44b8479469568a85c6b96"),
    ("scsg_ongaku_s20190914", f"{SITE}/download/scsg_ongaku_s20190914.zip",
     "182f829d1249f4103b5d6466788e8c5eed4054cb3e3b138f04404378c5924b2e"),
]

# optional analysis tools (--all)
EXTRA_TOOLS = [
    ("rom_checker_v154",      f"{SITE}/download/rom_checker_v154.zip",
     "987cdf011dae32cdcc2dc036cfdd1d8dfc8d1c7f2346759a59231da47dfca815"),
    ("nes_dis6502_v036",      f"{SITE}/download/nes_dis6502_v036.zip",
     "014627f6897f0b65a7e53c6f20680aaed0ba492be905487ee10720964a517609"),
    ("fds_dis6502_v021",      f"{SITE}/download/fds_dis6502_v021.zip",
     "344fc6b712d65bc1f51f467f3daeeafd4027d7a739bef81a0a94a4df467354e4"),
    ("snes_dis65816_v022",    f"{SITE}/download/snes_dis65816_v022.zip",
     "73fde128c164dad43437cfd7fba25d90b7ef027e37e4e9351fd4fdef51e64f0d"),
    ("snes_spc700_v013",      f"{SITE}/download/snes_spc700_v013.zip",
     "7335df5bbd1ed73e6bec069aca402e3c07f87678493cdda0658bd8c104f36a79"),
    ("gb_disz80c_v033",       f"{SITE}/download/gb_disz80c_v033.zip",
     "3cbff7b9ba2bccead0b1637775d1d3833388d995cec173aebc6ef18b07310cfe"),
    ("pce_dis6280_v036",      f"{SITE}/download/pce_dis6280_v036.zip",
     "afb2f22c001fe0010da8fcd5e543240735eb2356ddc24d567c79196b846243ce"),
    ("sms_disz80_v037",       f"{SITE}/download/sms_disz80_v037.zip",
     "71abbb8d108ba920bdfa9b5238498cc6593a3e8d5b55f9f70c3943225ae6de0d"),
    ("msx_disz80_v037",       f"{SITE}/download/msx_disz80_v037.zip",
     "c0e25735919ba8cf86b6400ac5a4f546e8d4107f2dea2d81694e993fa944c4e0"),
]

PAGES = ["index.html", "music/gb_ongaku.html", "music/famicom_ongaku.html",
         "music/pce_ongaku.html", "music/z80_ongaku.html"]

# expected row counts (^digit+\t) for post-extraction validation
EXPECTED_ROWS = {
    "GB_gbs2": 4254, "GB_gbs2_op": 1277, "GB_gbs2_op_title": 42,
    "NES_nsf_cart": 1790, "NES_fds_v2": 485, "NES_fds_v21": 638,
    "NES_fds_hacker": 70, "PCE_hes": 847, "MSX_kss": 1339,
    "GG_kss_k": 319, "GG_sgc_s": 479, "MK3_kss_k": 104, "MK3_sgc_s": 104,
    "SCSG_kss_k": 128, "SCSG_sgc_s": 171,
}

# resource-key -> output base name (prefix per app added by caller)
RESOURCE_MAP = {
    "GBgbs2": "GB_gbs2", "GBgbs2_op": "GB_gbs2_op", "GBgbs2_op_TITLE": "GB_gbs2_op_title",
    "nsf_21_nes_v2": "NES_nsf_cart", "nsf_22_fds_v2": "NES_fds_v2",
    "nsf_22_fds_v21": "NES_fds_v21", "nsf_23_fds_hacker_v2": "NES_fds_hacker",
    "PCEHES2emu": "PCE_hes", "MSX_kss2": "MSX_kss",
    "GG_kss2": "GG_kss", "MK3_kss2": "MK3_kss", "SCSG_kss2": "SCSG_kss",
}

DUMPRESource_CS = r"""
using System;
using System.IO;
using System.Collections;
using System.Resources;
using System.Reflection;
class D {
  static void Main(string[] a) {
    Assembly asm = Assembly.LoadFile(Path.GetFullPath(a[0]));
    Directory.CreateDirectory(a[1]);
    foreach (string rn in asm.GetManifestResourceNames()) {
      using (Stream rs = asm.GetManifestResourceStream(rn))
      using (var rr = new ResourceReader(rs)) {
        foreach (DictionaryEntry e in rr) {
          string k = (string)e.Key;
          string safe = "";
          foreach (char c in k) safe += (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '_' ? c.ToString() : "_";
          if (e.Value is string s) { File.WriteAllText(a[1] + "/" + safe + ".txt", s);
            Console.WriteLine(safe + " -> string " + s.Length); }
          else if (e.Value is byte[] b) { File.WriteAllBytes(a[1] + "/" + safe + ".bin", b);
            Console.WriteLine(safe + " -> bytes " + b.Length); }
        }
      }
    }
  }
}
"""


def log(msg):
    print(msg, flush=True)


def fetch(url, dest, sha=None, strict=False, tries=3):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists():
        got = hashlib.sha256(dest.read_bytes()).hexdigest()
        if sha is None or got == sha:
            log(f"  caché  {dest.name}")
            return True
        if not strict:
            log(f"  re-télécharge (empreinte différente) {dest.name}")
        else:
            log(f"  ERREUR empreinte {dest.name}: {got}")
            return False
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "memscan-bab-fetch/1.0"})
            with urllib.request.urlopen(req, timeout=60) as r, open(dest, "wb") as f:
                shutil.copyfileobj(r, f)
            got = hashlib.sha256(dest.read_bytes()).hexdigest()
            if sha and got != sha:
                log(f"  empreinte inattendue {dest.name} ({got[:12]}…)"
                    + (" — strict: rejeté" if strict else " — conservé"))
                if strict:
                    dest.unlink()
                    return False
            log(f"  ok     {dest.name} ({dest.stat().st_size} o)")
            return True
        except Exception as e:
            log(f"  essai {i+1}/{tries} échoué: {e}")
    return dest.exists()


def extract_archive(archive, outdir):
    outdir.mkdir(parents=True, exist_ok=True)
    if archive.suffix == ".zip":
        try:
            with zipfile.ZipFile(archive) as z:
                for info in z.infolist():
                    # zip stores cp932 names without the utf-8 flag; decode back
                    name = info.filename
                    try:
                        name = info.filename.encode("cp437").decode("cp932")
                    except (UnicodeDecodeError, UnicodeEncodeError):
                        pass
                    target = outdir / name
                    if info.is_dir():
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(z.read(info))
            return True
        except zipfile.BadZipFile:
            pass
    r = subprocess.run(["7z", "x", f"-o{outdir}", "-y", str(archive)],
                       capture_output=True)
    return r.returncode == 0


def find_exe(outdir):
    return sorted(p for p in outdir.rglob("*.exe") if p.stat().st_size > 10000)


def count_rows(text):
    return sum(1 for l in re.split("\r\n|\n", text) if re.match(r"^\d+\t", l))


def db_out_name(key, app_tag):
    """Canonical output name for a resource key under app tag (GG_K, GB, ...)."""
    base = re.sub(r"_[KS]$", "", app_tag)
    variant = app_tag[len(base) + 1:].lower() if len(app_tag) > len(base) else None
    stripped = key[len(base) + 1:] if key.upper().startswith(base.upper() + "_") else key
    name = RESOURCE_MAP.get(key) or RESOURCE_MAP.get(stripped) or f"{base}_{stripped.lower()}"
    return f"{name}_{variant}" if variant else name


def try_mono_resources(app_tag, exe, workdir, dbdir, manifest):
    """Preferred path: compile a tiny ResourceReader dumper with mcs, run mono.

    Returns the method name ("mono") on success, None otherwise.
    """
    build = workdir / "build"
    build.mkdir(parents=True, exist_ok=True)
    cs = build / "dumpres.cs"
    exe_out = build / "dumpres.exe"
    cs.write_text(DUMPRESource_CS)
    r = subprocess.run(["mcs", f"-out:{exe_out}", str(cs)], capture_output=True)
    if r.returncode != 0:
        return None
    # ASCII copy: LoadFile is finicky with non-ASCII paths
    exe_copy = build / f"{app_tag}.exe"
    shutil.copyfile(exe, exe_copy)
    out = workdir / f"res_{app_tag}"
    out.mkdir(parents=True, exist_ok=True)
    r = subprocess.run(["mono", str(exe_out), str(exe_copy), str(out)],
                       capture_output=True, text=True)
    if r.returncode != 0 or not out.exists():
        return None
    found = False
    for line in r.stdout.splitlines():
        m = re.match(r"^(\S+) -> (string|bytes) (\d+)$", line.strip())
        if not m:
            continue
        key, kind, size = m.group(1), m.group(2), int(m.group(3))
        src = out / f"{key}.{'txt' if kind == 'string' else 'bin'}"
        name = db_out_name(key, app_tag)
        if kind == "string" and size > 1500:
            text = src.read_text(encoding="utf-8-sig", errors="replace")
            rows = count_rows(text)
            if rows == 0:
                continue  # UI text, not a database
            (dbdir / f"{name}.tsv").write_text(text, encoding="utf-8")
            manifest["dbs"][name] = {
                "rows": rows, "chars": len(text),
                "source": f"{app_tag}.exe::{key}", "method": "mono",
            }
            found = True
        elif kind == "bytes" and size > 16:
            target = dbdir / f"{name}.bin"  # player stubs (PCE RST2F/RST3F...)
            shutil.copyfile(src, target)
            manifest["dbs"][name] = {"bytes": size, "source": f"{app_tag}.exe::{key}",
                                     "method": "mono"}
    return "mono" if found else None


def try_carve_resources(db_name, exe, dbdir, manifest):
    """Fallback without mono: carve the plaintext UTF-8 TSV blob out of the exe.

    Returns "carve" on success, None otherwise. Only handles the main DB of
    each app (the one whose header contains the RIP/CRC columns); the NES
    databases have no such header and need the mono path.
    """
    data = exe.read_bytes()
    pos = None
    for pat in (b"\tRIP\tCRC1", b"\tRIPING\tCRC1", b"RIP\tCRC1"):
        pos = data.find(pat)
        if pos >= 0:
            break
    if pos is None:
        return None
    # header line starts after the previous \n (or blob start)
    start = data.rfind(b"\n", 0, pos)
    start = pos if start < 0 else start + 1
    end = pos
    while end < len(data):
        c = data[end]
        if c in (9, 10, 13, 0x20) or 0x21 <= c < 0x7F or c >= 0x80:
            end += 1
        else:
            break
    text = data[start:end].decode("utf-8", "replace")
    lines = text.split("\r\n")
    while lines and not re.match(r"^[\wVNo ]*\t", lines[0]):
        lines.pop(0)
    text = "\r\n".join(lines)
    rows = count_rows(text)
    if rows < 10:
        return None
    (dbdir / f"{db_name}.tsv").write_text(text, encoding="utf-8")
    manifest["dbs"][db_name] = {"rows": rows, "chars": len(text),
                                "source": f"{db_name}.exe", "method": "carve"}
    return "carve"


def normalize_legacy(datfile, dbdir, manifest):
    raw = datfile.read_bytes()
    text = raw.decode("shift-jis", "replace")
    rows = list(csv.reader(io.StringIO(text)))
    out = dbdir / "GB_gbs2010.tsv"
    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f, delimiter="\t", lineterminator="\r\n")
        w.writerows(rows)
    manifest["dbs"]["GB_gbs2010"] = {"rows": len(rows) - 1,
                                     "source": "GBgbsV10/GBSDAT/GBgbs.dat (2010)",
                                     "method": "lha+7z+iconv"}
    return len(rows) - 1


README_FR = """\
# Bases locales BAB (mrchecker.web.fc2.com) — NE PAS REDISTRIBUER

Généré par `tools/bab_fetch.py`. Contenu téléchargé depuis le site de
l'auteur (BAB / GBackUR-BA) — freeware, usage personnel, redistribution
interdite sans son accord. Comme les ROMs : ces fichiers restent hors du
dépôt (output/ est ignoré par git).

## Contenu

- `db/*.tsv` : bases extraites des exécutables (TSV UTF-8), une ligne par jeu.
- `zip/`, `apps/` : archives d'origine et exécutables extraits.
- `manifest.json` : provenance, empreintes, comptages.

## Usage conforme au projet

Oracle de validation local uniquement : croiser les plages annoncées
(moteur audio, table de chansons, patches) avec les verdicts deepeel.
Toute connaissance qui entre dans le dépôt doit être re-dérivée par
l'explorateur et documentée comme observation de l'outil, jamais copiée
de ces bases. Les rapports publiés citent des agrégats ou des cas
re-dérivés, pas les lignes de ces bases.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default="output/bab", help="destination (default output/bab)")
    ap.add_argument("--all", action="store_true", help="aussi les désassembleurs et ROM Checker")
    ap.add_argument("--no-legacy", action="store_true", help="pas la base 2010 GBgbs")
    ap.add_argument("--strict", action="store_true", help="échoue si une empreinte diffère")
    ap.add_argument("--no-download", action="store_true", help="réutilise uniquement le cache")
    args = ap.parse_args()

    for tool in ("7z",):
        if not shutil.which(tool):
            sys.exit(f"{tool} requis")
    dest = Path(args.dest)
    zipdir, appdir, dbdir = dest / "zip", dest / "apps", dest / "db"
    for d in (zipdir, appdir, dbdir):
        d.mkdir(parents=True, exist_ok=True)

    manifest = {"site": SITE, "dbs": {}, "archives": {}}
    tools = list(MUSIC_TOOLS) + (list(EXTRA_TOOLS) if args.all else [])

    log("== téléchargements ==")
    for name, url, sha in tools:
        z = zipdir / f"{name}.zip"
        if args.no_download and not z.exists():
            log(f"  absent du cache: {z.name}")
            continue
        if not fetch(url, z, sha, args.strict):
            log(f"  ÉCHEC {name}")
            continue
        manifest["archives"][name] = {"url": url, "sha256": sha}
        adir = appdir / name
        if not adir.exists() or not find_exe(adir):
            extract_archive(z, adir)
            log(f"  extrait {name}")

    if not args.no_legacy:
        log("== base héritée GBgbs 2010 ==")
        lzh = zipdir / "GBgbsV10.lzh"
        if fetch(LEGACY_URL, lzh, LEGACY_SHA256, args.strict):
            manifest["archives"]["GBgbsV10"] = {"url": LEGACY_URL,
                                                "sha256": LEGACY_SHA256}
            adir = appdir / "GBgbsV10"
            if not (adir / "GBgbsV10").exists():
                extract_archive(lzh, adir)
            dat = next(adir.rglob("GBgbs.dat"), None)
            if dat:
                n = normalize_legacy(dat, dbdir, manifest)
                log(f"  GB_gbs2010.tsv: {n} entrées")
            else:
                log("  GBgbs.dat introuvable après extraction")

    log("== extraction des bases embarquées ==")
    have_mono = shutil.which("mono") and shutil.which("mcs")
    if not have_mono:
        log("  (mono/mcs absent -> repli par sculpture d'octets, bases NES indisponibles)")
    # zip name -> (resource tag, carve fallback DB name)
    EXTRACT_PLAN = {
        "gb_ongaku_v20220606": ("GB", "GB_gbs2"),
        "famion_v2_v20220606": ("NES", None),
        "pce_ongaku_v20220606": ("PCE", "PCE_hes"),
        "msx_ongaku_v20201121": ("MSX", "MSX_kss"),
        "gg_ongaku_k20220611": ("GG_K", "GG_kss_k"),
        "gg_ongaku_s20220611": ("GG_S", "GG_kss_s"),
        "mk3_ongaku_k20220219": ("MK3_K", "MK3_kss_k"),
        "mk3_ongaku_s20220219": ("MK3_S", "MK3_kss_s"),
        "scsg_ongaku_k20190914": ("SCSG_K", "SCSG_kss_k"),
        "scsg_ongaku_s20190914": ("SCSG_S", "SCSG_kss_s"),
    }
    for name, _url, _sha in tools:
        plan = EXTRACT_PLAN.get(name)
        if plan is None:
            continue  # analysis tools (--all) carry no databases
        tag, carve_name = plan
        adir = appdir / name
        exes = find_exe(adir)
        if not exes:
            log(f"  {tag}: aucun exe extrait")
            continue
        exe = exes[0]
        method = try_mono_resources(tag, exe, dest, dbdir, manifest) if have_mono else None
        if method is None:
            method = try_carve_resources(carve_name, exe, dbdir, manifest) if carve_name else None
        log(f"  {tag}: {method or 'ÉCHEC'}")

    log("== pages web (provenance) ==")
    pages = dest / "pages"
    for p in PAGES:
        fetch(f"{SITE}/{p}", pages / Path(p).name, None, False)

    # validation
    log("== validation ==")
    ok = True
    for name, info in sorted(manifest["dbs"].items()):
        want = EXPECTED_ROWS.get(name)
        mark = ""
        if want and info.get("rows") not in (None, want):
            mark = f"  ATTENDU {want}"; ok = False
        if "rows" in info:
            log(f"  {name}: {info['rows']} lignes{mark}")
        else:
            log(f"  {name}: {info.get('bytes')} o")
    missing = [n for n in EXPECTED_ROWS if n not in manifest["dbs"]]
    if missing:
        log(f"  manquants: {', '.join(missing)}" + ("  (mono requis ?)" if not have_mono else ""))
        ok = False

    (dest / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False),
                                        encoding="utf-8")
    readme = dest / "README"
    if not readme.exists():
        readme.write_text(README_FR, encoding="utf-8")
    log(f"== manifest: {dest / 'manifest.json'} — {'OK' if ok else 'INCOMPLET'} ==")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

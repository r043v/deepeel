#!/usr/bin/env python3
"""Bulk-fetch a joshw.info rip collection into the local rip library.

Walks the letter listings of the joshw.info sites (the hcs64 community rip
archive maintained by JoshW — no verified real name, no license page: the rip
data stays in output/, local oracle only, like the BAB databases,
one site per format), downloads every *.7z, extracts the rip files and deletes
the archive: output/joshw/<site>/<segment>/<archive>/ keeps only the rips.
Resumable — an archive whose marker exists is skipped, so the script just
re-runs.  `music_index.py build --rip-dirs output/gbs output/joshw` ingests the
whole library (origin joshw:<site>!..., source joshw_*).

Default sites are the engine-rip formats deepeel can seed from (load/init/play
entry points the verifier can map): gbs (GB), nsfe (NES), hes (PCE), kss
(MSX + Sega Mark III/SMS/GG).  spc (SNES) and vgm (MD/arcade) are logged
streams without entry points — nothing to verify or seed, and gigabytes: pass
them explicitly if the library is wanted anyway.

Politeness: sequential downloads, backoff on 503, UA deepeel-joshw/1.  Stops
before the disk goes under --min-free MB (the archive just fetched is kept as
the resume point)."""
import argparse
import html
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.parse import unquote
from urllib.request import Request, urlopen

SITES = {
    "gbs": "GB",
    "nsfe": "NES",
    "hes": "PCE",
    "kss": "MSX/Sega",
    "spc": "SNES (no entry points, big)",
    "vgm": "MD/arcade (no entry points, big)",
}
UA = "deepeel-joshw/1"
RIP_EXTS = {".gbs", ".nsf", ".nsfe", ".kss", ".hes", ".spc", ".vgm", ".vgz", ".ay", ".sap", ".sgc"}
SEGMENTS = [chr(c) for c in range(ord("a"), ord("z") + 1)] + ["0-9"]


def fetch(url, tries=6):
    for t in range(tries):
        try:
            req = Request(url, headers={"User-Agent": UA})
            with urlopen(req, timeout=180) as r:
                return r.read()
        except HTTPError as e:
            if e.code == 404:
                return None
            time.sleep(2 * (t + 1))          # the site answers 503 under load
        except (URLError, TimeoutError, OSError):
            time.sleep(2 * (t + 1))
    return None


def have_rip(d: Path) -> bool:
    return d.is_dir() and any(p.suffix.lower() in RIP_EXTS for p in d.iterdir())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sites", default="gbs,nsfe,hes,kss",
                    help=f"comma list from {sorted(SITES)} (default: the seedable formats)")
    ap.add_argument("--out", default="output/joshw")
    ap.add_argument("--min-free", type=int, default=2000,
                    help="stop when the output filesystem drops under this many MB free")
    ap.add_argument("--sleep", type=float, default=0.4, help="seconds between downloads")
    args = ap.parse_args()

    for site in args.sites.split(","):
        if site not in SITES:
            sys.exit(f"unknown site {site} (choices: {sorted(SITES)})")

    total = done = skip = fail = 0
    Path(args.out).mkdir(parents=True, exist_ok=True)
    for site in args.sites.split(","):
        root = Path(args.out) / f"{site}.joshw.info"
        for seg in SEGMENTS:
            hrefs = listing_hrefs(f"https://{site}.joshw.info/{seg}/")
            if not hrefs:
                continue
            print(f"[{site}/{seg}] {len(hrefs)} archives", flush=True)
            for href in hrefs:
                name = unquote(html.unescape(href))   # hrefs carry &amp; etc.
                if shutil.disk_usage(args.out).free < args.min_free * (1 << 20):
                    sys.exit(f"disk under {args.min_free} MB free — re-run to resume")
                arcdir = root / seg / re.sub(r"\.7z$", "", name, flags=re.I)
                if have_rip(arcdir) or (arcdir / ".done").exists():
                    skip += 1
                    continue
                total += 1
                data = fetch(f"https://{site}.joshw.info/{seg}/{html.unescape(href)}")
                if not data:
                    print(f"  FAIL {name}", flush=True)
                    fail += 1
                    continue
                arcdir.mkdir(parents=True, exist_ok=True)
                tmp = arcdir / "archive.7z"
                tmp.write_bytes(data)
                subprocess.run(["7z", "x", "-y", f"-o{arcdir}", str(tmp)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                tmp.unlink()
                for p in list(arcdir.rglob("*")):
                    if p.is_file() and p.suffix.lower() not in RIP_EXTS:
                        p.unlink()           # keep only the rips, like --gbs-fetch
                (arcdir / ".done").touch()
                done += 1
                if done % 25 == 0:
                    print(f"  {done} fetched ({skip} skipped, {fail} failed)", flush=True)
                time.sleep(args.sleep)
    print(f"joshw_fetch: {done} fetched, {skip} already there, {fail} failed", flush=True)


def listing_hrefs(base):
    h = fetch(base)
    if not h:
        return []
    return [m.decode() for m in re.findall(rb'href="([^"]+\.7z)"', h, re.I)]


if __name__ == "__main__":
    main()

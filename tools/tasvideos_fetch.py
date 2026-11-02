#!/usr/bin/env python3 -I
"""Search TASVideos for Game Boy movies matching a ROM and download them.

usage: tasvideos_fetch.py ROM.gb OUT_DIR [--max N] [--list] [--game "TASVideos game name"] [--system GB|NES|MD|...]
--game is needed when the ROM file name differs from the TASVideos title.
Each movie goes in its own fresh sub-directory of OUT_DIR (downloads are
untrusted data). Prints the --tas arguments to give to deepeel (at most 8).
"""
import io, json, os, re, sys, urllib.request, zipfile

def api(system="GB"):
    return ("https://tasvideos.org/api/v1/publications?Systems=%s"
            "&PageSize=100" % system)
DL = "https://tasvideos.org/%dM?handler=Download"
EXT = (".vbm", ".bk2", ".gbm", ".fm2")

def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "deepeel-tasfetch/1"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()

def norm(s):
    return re.sub(r"[^a-z0-9]+", " ", s.lower()).strip()

def rom_title(path):
    with open(path, "rb") as f:
        h = f.read(0x150)
    return h[0x134:0x144].split(b"\0")[0].decode("ascii", "replace").strip()

def main():
    argv = sys.argv[1:]
    game = None
    system = "GB"
    if "--game" in argv:
        i = argv.index("--game")
        game = argv[i + 1]
        del argv[i:i + 2]
    if "--system" in argv:
        i = argv.index("--system")
        system = argv[i + 1]
        del argv[i:i + 2]
    a = [x for x in argv if not x.startswith("--")]
    mx = 8
    if "--max" in sys.argv:
        mx = min(8, int(argv[argv.index("--max") + 1]))
        a = [x for x in a if x != argv[argv.index("--max") + 1]]
    if len(a) < 2:
        sys.exit(__doc__)
    rom, out = a[0], a[1]
    title = norm(game or re.sub(r"\(.*?\)", "", os.path.basename(rom).rsplit(".", 1)[0]))
    pubs = json.loads(get(api(system)))
    pubs = pubs if isinstance(pubs, list) else pubs.get("items", [])
    hits = []
    for p in pubs:
        g = re.sub(r"^%s\s+" % re.escape(system), "", p.get("title", ""))
        g = re.split(r"\s+by\s+", g)[0]  # drop the author
        g = re.sub(r'\s*"[^"]*"\s*$', "", g)  # drop a quoted category, keep the subtitle
        if title and norm(g) == title:
            hits.append(p)
    print("rom header title: %r; %d matching publication(s)" % (rom_title(rom), len(hits)), file=sys.stderr)
    for p in hits:
        print("  %s  %s" % (p.get("id"), p.get("title")), file=sys.stderr)
    if "--list" in sys.argv:
        return
    args = []
    for p in hits[:mx]:
        d = os.path.join(out, str(p["id"]))
        os.makedirs(d, exist_ok=False) if not os.path.isdir(d) else None
        try:
            blob = get(DL % p["id"])
            z = zipfile.ZipFile(io.BytesIO(blob))
        except Exception as e:
            print("  %s: download failed (%s)" % (p["id"], e), file=sys.stderr)
            continue
        for n in z.namelist():
            if n.lower().endswith(EXT) and "/" not in n.strip("/") :
                fp = os.path.join(d, os.path.basename(n))
                with open(fp, "wb") as f:
                    f.write(z.read(n))
                args.append(fp)
    print(" ".join("--tas " + x for x in args))

main()

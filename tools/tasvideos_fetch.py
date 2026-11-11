#!/usr/bin/env python3 -I
"""Download every TASVideos movie of a Game Boy (or other) game: all of them is the norm.

usage: tasvideos_fetch.py ROM OUT_DIR [--game-id N[,N...]] [--game "TASVideos game name"]
                          [--system GB|NES|MD|...] [--max N] [--list]

Order: publications first (the game page https://tasvideos.org/NNNG, then the
publications API), then the submissions.  --game-id takes several ids (comma list).
Sources, all kept (no cap unless --max): the Subs-List of the game (every
submission whatever its status: published, rejected, cancelled, playground...
free data, often more recorders and routes than the publications) and the
publications of the game.  --game-id is the TASVideos GameId (Subs-List?GameId=N);
without it the id is looked up from the publications of the game.
Each movie goes in its own fresh sub-directory of OUT_DIR (downloads are
untrusted data; an existing sub-directory is skipped).  Prints the --tas
arguments to give to deepeel (it replays them all, queueing beyond its workers).
"""
import io, json, os, re, sys, time, urllib.request, zipfile

SUBS = "https://tasvideos.org/Subs-List?GameId=%d"
PUBS = "https://tasvideos.org/api/v1/publications?Systems=%s&PageSize=100"
EXT = (".vbm", ".bk2", ".gbm", ".fm2", ".gmv", ".fm3", ".mc2", ".mmv", ".r08", ".lsmv")

def get(url, tries=6):
    err = None
    for k in range(tries):  # the site answers 503 under load: back off and retry
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "deepeel-tasfetch/2"})
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.read()
        except Exception as e:
            err = e
            time.sleep(2 * (k + 1))
    raise err

def norm(s):
    return re.sub(r"[^a-z0-9]+", " ", s.lower()).strip()

def rom_title(path):
    try:
        with open(path, "rb") as f:
            h = f.read(0x150)
    except OSError:
        return os.path.basename(path)   # listing without a ROM
    if len(h) < 0x150 or not path.lower().endswith((".gb", ".gbc")):
        return os.path.basename(path)   # only GB headers carry a title
    return h[0x134:0x144].split(b"\0")[0].decode("ascii", "replace").strip()

def opt(argv, name, default=None):
    if name in argv:
        i = argv.index(name)
        v = argv[i + 1]
        del argv[i:i + 2]
        return v
    return default

ALT_SYSTEM = {"MD": ("Genesis",)}   # the publications API renamed the system codes (MD -> Genesis, observed 11/2026)

def pubs_of(system, title):
    hits = []
    prefixes = [system] + list(ALT_SYSTEM.get(system, ()))
    for sys_ in prefixes:
        data = json.loads(get(PUBS % sys_))
        data = data if isinstance(data, list) else data.get("items", [])
        for p in data:
            g = p.get("title", "")
            for pre in prefixes:   # the titles carry the system word ("Genesis Sonic ...")
                g = re.sub(r"^%s\s+" % re.escape(pre), "", g, count=1, flags=re.I)
            g = re.split(r"\s+by\s+", g)[0]
            g = re.sub(r'\s*"[^"]*"\s*$', "", g)
            if title and norm(g) == title:
                hits.append(p)
        if hits:
            break
    return hits

def main():
    argv = sys.argv[1:]
    game = opt(argv, "--game")
    system = opt(argv, "--system", "GB")
    gid = opt(argv, "--game-id")
    mx = opt(argv, "--max")
    lst = "--list" in argv
    a = [x for x in argv if not x.startswith("--")]
    if len(a) < 2:
        sys.exit(__doc__)
    rom, out = a[0], a[1]
    title = norm(game or re.sub(r"\(.*?\)", "", os.path.basename(rom).rsplit(".", 1)[0]))
    pubs = []
    try:
        pubs = pubs_of(system, title)
    except Exception as e:
        print("publications lookup failed (%s)" % e, file=sys.stderr)
    if gid is None:
        for p in pubs:
            for k in ("gameId", "game_id", "GameId"):
                if p.get(k):
                    gid = p[k]
                    break
            if gid:
                break
    items = []  # (kind, id, label, download url): publications first, then submissions
    gids = [int(x) for x in str(gid).split(",") if x.strip()] if gid else []
    pseen = set(p["id"] for p in pubs)
    for g in gids:   # the game page lists its publications whatever the API's title matching finds
        try:
            ghtml = get("https://tasvideos.org/%dG" % g).decode("utf-8", "replace")
        except Exception as e:
            print("game page %dG failed (%s)" % (g, e), file=sys.stderr)
            continue
        for m in re.finditer(r'href="/(\d+)M"[^>]*>([^<]*)', ghtml):
            if int(m.group(1)) not in pseen:
                pseen.add(int(m.group(1)))
                pubs.append({"id": int(m.group(1)), "title": m.group(2).strip()})
    for p in pubs:
        items.append(("M", p["id"], p.get("title", ""),
                      "https://tasvideos.org/%dM?handler=Download" % p["id"]))
    for gid in gids:
        html = get(SUBS % gid).decode("utf-8", "replace")
        seen = set()
        for row in re.findall(r"<tr>(.*?)</tr>", html, re.S):
            m = re.search(r'href="/(\d+)S"', row)
            if not m or m.group(1) in seen:
                continue
            seen.add(m.group(1))
            cells = [re.sub(r"\s+", " ", re.sub(r"<.*?>", " ", c)).strip()
                     for c in re.findall(r"<td.*?>(.*?)</td>", row, re.S)]
            items.append(("S", int(m.group(1)), " | ".join(cells[2:7]),
                          "https://tasvideos.org/%sS?handler=Download" % m.group(1)))
    if not gids:
        print("no GameId (pass --game-id N from Subs-List?GameId=N): publications only", file=sys.stderr)
    print("rom header title: %r; %d submission/publication movie(s)" % (rom_title(rom), len(items)),
          file=sys.stderr)
    for k, i, lab, _ in items:
        print("  %d%s  %s" % (i, k, lab), file=sys.stderr)
    if lst:
        return
    if mx:
        items = items[:int(mx)]
    args = []
    for k, i, lab, url in items:
        d = os.path.join(out, "%d%s" % (i, k))
        if os.path.isdir(d):
            print("  %d%s: already there, skipped" % (i, k), file=sys.stderr)
            args += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.lower().endswith(EXT)]
            continue
        try:
            z = zipfile.ZipFile(io.BytesIO(get(url)))
        except Exception as e:
            print("  %d%s: download failed (%s)" % (i, k, e), file=sys.stderr)
            continue
        os.makedirs(d)
        for n in z.namelist():
            if n.lower().endswith(EXT) and not n.endswith("/"):
                fp = os.path.join(d, os.path.basename(n))
                with open(fp, "wb") as f:
                    f.write(z.read(n))
                args.append(fp)
    print(" ".join("--tas " + x for x in args))

main()

#!/usr/bin/env python3 -I
# romset_batch.py: full-coverage batch of deepeel over a zipped Game Boy romset.
#
# For every zip of the romset: copy it into roms/gb/<subdir>/ of the repository,
# fetch the TASVideos movies of the game (published AND submissions, rejected
# ones included), run deepeel --unbounded in time slices on scratch storage
# (the Windows drive), stop on exhaustion / negligible gain / cap, keep the
# map PNG and the final reports under results/gb/<subdir>/<slug>/, then wipe
# the scratch directory.  results/gb/ROMSET.{log,csv,json} accumulate the
# stats of the whole set; index.json makes the batch resumable.
#
# Every number is an observation of the tool (concrete machine, zeroed RAM),
# not a hardware measurement.  The TAS verdict is an estimate (deepeel has no
# desync detector): raw values are kept so the heuristic can be revisited.
#
# usage: romset_batch.py [--roms DIR] [--jobs-roms N] [--slice SEC] [--cap SEC]
#        [--only SUBSTR] [--limit N] [--mapwin] [--dry-run]
import argparse, csv, json, os, re, shutil, signal, subprocess, sys, threading, time
import urllib.request, zipfile
from concurrent.futures import ThreadPoolExecutor

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MOVIE_EXTS = (".vbm", ".vmv", ".bk2")
API = "https://tasvideos.org/api/v1/submissions?Systems=%s&PageSize=100&CurrentPage=%d"
DL = "https://tasvideos.org/%dS?handler=Download"
LOCK = threading.Lock()
STOP = threading.Event()


def norm(s):
    s = re.sub(r"\(.*?\)|\[.*?\]", " ", s or "")
    s = re.sub(r"^(.*?),\s*The\b(.*)$", r"The \1\2", s, flags=re.I)
    s = re.sub(r"^the\s+", "", s.strip(), flags=re.I)
    return re.sub(r"[^a-z0-9]", "", s.lower())


def slugify(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s).strip("-").lower()[:70] or "rom"


def http(url):
    req = urllib.request.Request(url, headers={"User-Agent": "deepeel-romset-batch/1"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def load_catalog(path):
    """every GB/GBC submission of TASVideos (all statuses), cached on disk"""
    if os.path.exists(path):
        return json.load(open(path))
    cat = []
    for system in ("GB", "GBC"):
        page = 1
        while True:
            items = json.loads(http(API % (system, page)))
            if not items:
                break
            cat += [{k: it.get(k) for k in ("id", "title", "gameName", "gameVersion", "romName", "status",
                                            "movieExtension", "frames", "systemCode", "emulatorVersion")}
                    for it in items]
            page += 1
    os.makedirs(os.path.dirname(path), exist_ok=True)
    json.dump(cat, open(path, "w"))
    return cat


def regions(name):
    """region letters of the first parenthesised tag: (USA, Europe) -> UE, (UE) -> UE, (J) -> J; '' if unknown"""
    m = re.search(r"\(([^)]*)\)", name or "")
    if not m:
        return set()
    t = m.group(1)
    if re.fullmatch(r"[A-Z]{1,4}", t):
        return set(t)
    return {w[0] for w in re.split(r"[ ,]+", t) if w[:1].isupper() and w in
            ("USA", "Europe", "Japan", "World", "Germany", "France", "Spain", "Italy", "Korea", "Australia")}


def pick_movies(cat, rom_base, limit=8):
    key = norm(rom_base)
    rr = regions(rom_base)
    # a movie recorded on another region's ROM is refused by the engine and would be a wasted replay (every movie is replayed, queued beyond the workers)
    hits = [c for c in cat if key and norm(c["gameName"]) == key
            and (c.get("movieExtension") or "").lower() in [e[1:] for e in MOVIE_EXTS]
            and (not rr or not regions(c.get("gameVersion")) or rr & regions(c.get("gameVersion")))]
    hits.sort(key=lambda c: (c["status"] != "Published", -(c.get("frames") or 0)))
    return hits[:limit]


def fetch_movies(hits, dest):
    """download each submission into its own fresh sub-directory (untrusted data)"""
    got = []
    for h in hits:
        d = os.path.join(dest, "m%d" % h["id"])
        os.makedirs(d, exist_ok=True)
        try:
            blob = http(DL % h["id"])
            tmp = os.path.join(d, "sub.zip")
            open(tmp, "wb").write(blob)
            with zipfile.ZipFile(tmp) as z:
                for zi in z.infolist():
                    base = os.path.basename(zi.filename)
                    if base.lower().endswith(MOVIE_EXTS) and zi.file_size < 64 << 20:
                        out = os.path.join(d, "%d-%s" % (h["id"], base))
                        with z.open(zi) as src, open(out, "wb") as dst:
                            shutil.copyfileobj(src, dst)
                        got.append({"path": out, "id": h["id"], "status": h["status"], "name": base,
                                    "title": h["title"], "sub_frames": h.get("frames")})
        except Exception as e:  # network or bad archive: the movie is simply skipped
            log("movie %s fetch failed: %s" % (h["id"], e))
    return got


def dir_size(p):
    t = 0
    for r, _d, fs in os.walk(p):
        for f in fs:
            try:
                t += os.path.getsize(os.path.join(r, f))
            except OSError:
                pass
    return t


def group_rss(pgid):
    """resident bytes of every process of the group (threads share their process)"""
    t = 0
    for d in os.listdir("/proc"):
        if not d.isdigit():
            continue
        try:
            st = open("/proc/%s/stat" % d).read().rsplit(")", 1)[1].split()
            if int(st[2]) == pgid:
                t += int(st[21]) * 4096
        except (OSError, IndexError, ValueError):
            pass
    return t


ACTIVE = {}              # pgid -> resident bytes of the running explorations (shared RAM pool)
ACTIVE_LOCK = threading.Lock()


def run_slice(cmd, logpath, state, seconds, guard, mem, pool=0):
    """one bounded deepeel run; SIGINT (graceful) on the size guard, hard kill as a last resort"""
    t0 = time.time()
    # hard RAM ceiling through a cgroup scope (no swap), graceful SIGINT below it
    cmd = ["systemd-run", "--user", "--scope", "-q", "-p", "MemoryMax=%d" % mem,
           "-p", "MemorySwapMax=0"] + cmd
    with open(logpath, "ab") as lf:
        p = subprocess.Popen(cmd, stdout=lf, stderr=subprocess.STDOUT, preexec_fn=os.setsid)
        why = "wall"
        tick = 0
        while True:
            try:
                p.wait(timeout=1)
                why = "exit"
                break
            except subprocess.TimeoutExpired:
                pass
            el = time.time() - t0
            if STOP.is_set():
                os.killpg(p.pid, signal.SIGINT)
                p.wait(timeout=120)
                why = "interrupted"
                break
            tick += 1
            rss = group_rss(p.pid)
            with ACTIVE_LOCK:
                ACTIVE[p.pid] = rss
                total = sum(ACTIVE.values())
                biggest = max(ACTIVE, key=ACTIVE.get)
            # own ceiling, or the shared pool is over budget and this one is the largest consumer
            if rss > mem * 0.8 or (pool and total > pool * 0.9 and biggest == p.pid):
                os.killpg(p.pid, signal.SIGINT)
                why = "mem-guard"
                try:
                    p.wait(timeout=180)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
                break
            if tick % 5 == 0 and shutil.disk_usage(state).free < guard:
                os.killpg(p.pid, signal.SIGINT)
                why = "state-guard"
                try:
                    p.wait(timeout=180)
                except subprocess.TimeoutExpired:
                    os.killpg(p.pid, signal.SIGKILL)
                break
            if el > seconds + 300:
                os.killpg(p.pid, signal.SIGKILL)
                why = "hard-kill"
                break
    with ACTIVE_LOCK:
        ACTIVE.pop(p.pid, None)
    return time.time() - t0, p.returncode, why


def tas_info(logtext):
    """raw TAS facts from the run log; the verdict is an estimate"""
    movies = {}
    for m in re.finditer(r"deepeel: TAS (.+?): (\d+) frames", logtext):
        movies[os.path.basename(m.group(1))] = {"frames": int(m.group(2))}
    refused = len(re.findall(r"TAS .+? refused: recorded on another ROM", logtext))
    iv = re.search(r"checkpoint snapshot every (\d+) frames", logtext)
    caps = re.search(r"(\d+) TAS checkpoint snapshots saved", logtext)
    interval = int(iv.group(1)) if iv else 1800
    return movies, refused, interval, int(caps.group(1)) if caps else 0


def discovery(summary):
    return {d["source"]: d for d in summary.get("discovery", [])}


def tas_verdict(n_movies, movies, refused, interval, captures, summary, wall_cut):
    """deepeel has no desync detector.  Estimate from what it logs: the frames
    the movie reached (checkpoints x interval, or the last credited frame) and
    the last frame where the movie still brought new evidence.  A movie that
    played to its end but stopped paying early (< 60% of its length) is
    reported as a probable desync; beyond that point its contribution is zero."""
    if not n_movies:
        return {"verdict": "no_movie"}
    if refused and not movies:
        return {"verdict": "rom_refused", "refused": refused}
    if not movies:
        return {"verdict": "not_run"}
    disc = discovery(summary)
    run_t = max([v.get("last_gain_t", 0) for v in disc.values()] + [1])   # proxy of the run length
    per = []
    for name, mv in movies.items():
        # summary.json truncates source names (~48 chars) and the movie name may carry odd spaces
        d = disc.get("tas:" + name) or next((v for k, v in disc.items() if k.startswith("tas:")
                                             and k != "tas:" and name.startswith(k[4:].rstrip())), {})
        total = mv["frames"]
        last_cf = d.get("last_credit_frame", 0)
        reached = max(captures * interval if len(movies) == 1 else 0, d.get("max_frame", 0))
        played_to_end = reached >= total - interval
        # movies are replayed first (seconds into the run): credit stopping well before the end while the
        # run went on means the replay left the recorded path (desync), not the wall clock
        late = bool(wall_cut) and d.get("last_gain_t", 0) > 0.9 * run_t
        if played_to_end and last_cf >= 0.6 * total:
            v = "complete"
        elif late:
            v = "cut_by_wall"
        elif last_cf and last_cf < 0.9 * total:
            v = "desync_est"
        else:
            v = "stopped_early"
        per.append({"movie": name, "frames_total": total, "frames_reached_est": reached,
                    "last_credit_frame": last_cf, "credit": d.get("credit", 0),
                    "desync_frame_est": last_cf if v == "desync_est" else None, "verdict": v})
    order = ["complete", "desync_est", "cut_by_wall", "stopped_early"]
    best = min(per, key=lambda x: order.index(x["verdict"]))
    return {"verdict": best["verdict"], "refused": refused, "per_movie": per,
            "counts": {v: sum(1 for x in per if x["verdict"] == v) for v in order}}


def log(msg):
    line = "%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    with LOCK:
        print(line, flush=True)
        with open(os.path.join(RES, "ROMSET.log"), "a") as f:
            f.write(line + "\n")


def process(e, cat, a):
    slug, zpath = e["slug"], e["repo_zip"]
    res_dir = os.path.join(RES, e["sub"], slug)
    tmp = os.path.join(a.scratch, slug)
    os.makedirs(res_dir, exist_ok=True)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    state = os.path.join(tmp, "state")
    os.makedirs(state)
    meta = {"slug": slug, "sub": e["sub"], "zip": e["rel"], "zip_bytes": e["size"], "slices": []}
    t_start = time.time()
    try:
        free = shutil.disk_usage(a.scratch).free
        if free < a.guard * 2:
            raise RuntimeError("scratch space low (%d MB free)" % (free >> 20))
        hits = pick_movies(cat, os.path.splitext(e["file"])[0]) if not a.no_tas else []
        movies = fetch_movies(hits, os.path.join(tmp, "tas")) if hits else []
        meta["movies"] = [{k: m[k] for k in ("id", "status", "name", "title", "sub_frames")} for m in movies]
        summ = os.path.join(res_dir, "summary.json")
        base = [a.binary, "explore", zpath, "--play", "--unbounded", "--jobs", str(a.jobs),
                "--state-dir", state, "--summary", summ, "--text", os.path.join(res_dir, "report.txt"),
                "--map", os.path.join(res_dir, "map.png"), "--closure-rounds", "12", "--seed-banks"]
        if a.sessions:
            base += ["--sessions", str(a.sessions)]
        if a.queue:
            base += ["--queue", str(a.queue), "--queue-max", str(a.queue)]   # no adaptive growth: bounded RAM   # states held in RAM before spilling to spool.bin
        if a.mapwin:
            base += ["--mapwin"]
        for m in movies:
            base += ["--tas", m["path"]]
        logpath = os.path.join(res_dir, "run.log")
        open(logpath, "w").close()
        # one run per ROM (a resource-guard stop resumes it: coverage.bin keeps the map).  The run
        # stops by itself on exhaustion, on --stagnation (no discovery for N
        # play frames = negligible gain) or on the --time-limit cap.
        cmd = base + ["--time-limit", str(int(a.cap)), "--stagnation", str(a.stagnation), "--until-static", "--search-limit", str(a.search)]
        el, rc, why = run_slice(cmd, logpath, state, a.cap + a.search, a.guard, int(a.ram_gb * (1 << 30) * 0.45), int(a.ram_gb * (1 << 30)))
        # a resource guard stops the run gracefully; the state dir now keeps the coverage map
        # (coverage.bin), so --resume continues the same exploration without losing what is known
        for _ in range(3):
            if why not in ("mem-guard", "state-guard") or STOP.is_set() or not os.path.exists(os.path.join(state, "coverage.bin")):
                break
            log("%s: %s, resuming from the saved state" % (slug, why))
            el2, rc, why = run_slice(cmd + ["--resume"], logpath, state, a.cap + a.search, a.guard,
                                     int(a.ram_gb * (1 << 30) * 0.45), int(a.ram_gb * (1 << 30)))
            el += el2
        s = json.load(open(summ)) if os.path.exists(summ) else {}
        reason = s.get("play", {}).get("stop_reason")
        wall_cut = (reason == "time budget reached")
        meta["slices"].append({"wall": round(el, 1), "rc": rc, "why": why, "stop_reason": reason,
                               "known_pct": s.get("map", {}).get("known_pct"),
                               "state_mb": dir_size(state) >> 20})
        log("%s run: %.0fs rc=%s %s reason=%s known %s%% state %d MB" %
            (slug, el, rc, why, reason, s.get("map", {}).get("known_pct"), meta["slices"][-1]["state_mb"]))
        if not s:
            stop = "error"
            try:
                if "no machine variant compiled" in open(logpath, errors="replace").read():
                    stop = "unsupported"   # cartridge type the engine has no machine for: not a failure of the run
            except OSError:
                pass
        elif why in ("state-guard", "hard-kill", "interrupted", "mem-guard"):
            stop = why
        elif wall_cut:
            stop = "cap"
        elif reason == "stagnation wall reached":
            stop = "negligible_gain"
        else:
            stop = "exhausted"
        meta["stop"] = stop
        s = json.load(open(summ)) if os.path.exists(summ) else {}
        logtext = open(logpath, errors="replace").read()
        txt = open(os.path.join(res_dir, "report.txt"), errors="replace").read() \
            if os.path.exists(os.path.join(res_dir, "report.txt")) else ""
        mv, refused, interval, captures = tas_info(logtext)
        meta["tas"] = tas_verdict(len(movies), mv, refused, interval, captures, s, wall_cut)
        meta["methods_pct"] = s.get("map", {}).get("methods_pct", {})
        meta["map"] = {k: s.get("map", {}).get(k) for k in ("known_pct", "code_pct", "data_pct", "pad_pct", "unknown_pct")}
        meta["counters"] = s.get("counters", {})
        meta["wall_s"] = round(time.time() - t_start, 1)
        meta["status"] = "done" if stop in ("exhausted", "negligible_gain") else \
            ("capped" if stop == "cap" else stop)
        # keep tiny logs only: the run log can grow, truncate its middle
        if os.path.getsize(logpath) > 2 << 20:
            data = open(logpath, "rb").read()
            open(logpath, "wb").write(data[:1 << 20] + b"\n[... truncated ...]\n" + data[-(1 << 20):])
    except Exception as ex:
        meta["status"] = "failed"
        meta["error"] = repr(ex)
        log("%s FAILED: %r" % (slug, ex))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    json.dump(meta, open(os.path.join(res_dir, "meta.json"), "w"), indent=1)
    return meta


def write_tables(metas):
    cols = ["slug", "sub", "status", "stop", "wall_s", "slices", "known_pct", "code_pct", "data_pct",
            "tas_pct", "random_pct", "fork_pct", "attract_pct", "static_pct", "movies", "tas_verdict",
            "tas_frames_total", "tas_frames_reached_est", "tas_last_credit_frame"]
    rows = []
    for m in metas:
        mp, mm, t = m.get("methods_pct", {}), m.get("map", {}), m.get("tas", {})
        rows.append([m["slug"], m["sub"], m.get("status"), m.get("stop"), m.get("wall_s"), len(m["slices"]),
                     mm.get("known_pct"), mm.get("code_pct"), mm.get("data_pct"),
                     mp.get("tas"), mp.get("random"), mp.get("fork"), mp.get("attract"), mp.get("static"),
                     len(m.get("movies", [])), t.get("verdict"), 
                     max((x["frames_total"] for x in t.get("per_movie", [])), default=None),
                     max((x["frames_reached_est"] for x in t.get("per_movie", [])), default=None),
                     max((x["last_credit_frame"] for x in t.get("per_movie", [])), default=None)])
    with LOCK:
        with open(os.path.join(RES, "ROMSET.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(cols)
            w.writerows(rows)
        json.dump(metas, open(os.path.join(RES, "ROMSET.json"), "w"))


def main():
    global RES
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.path.expanduser("~/Téléchargements/roms"))
    ap.add_argument("--binary", default=os.path.join(REPO, "build", "deepeel"))
    ap.add_argument("--scratch", default="/run/media/m/Windows/claude/romset/tmp")
    ap.add_argument("--results", default=os.path.join(REPO, "results", "gb"))
    ap.add_argument("--repo-roms", default=os.path.join(REPO, "roms", "gb"))
    ap.add_argument("--jobs-roms", type=int, default=4, help="ROMs explored in parallel (they share the --ram-gb pool; each also has a hard cgroup ceiling of 45 percent of it)")
    ap.add_argument("--jobs", type=int, default=8, help="--jobs of each deepeel (bursty load: 4 x 8 workers share the cores)")
    ap.add_argument("--ram-gb", type=float, default=10.0, help="total RAM ceiling for all deepeel runs (user limit: 10 GB)")
    ap.add_argument("--stagnation", type=int, default=30000, help="play frames without discovery before stopping")
    ap.add_argument("--cap", type=int, default=1200, help="max exploration wall per ROM (s)")
    ap.add_argument("--guard", type=int, default=15 << 30, help="stop a run only when the scratch disk has less than this many bytes free (the disk spool unloads the RAM; no per-ROM size cap)")
    ap.add_argument("--search", type=int, default=600, help="seconds of fork search after the play budget (deepeel --search-limit); 0 = skip")
    ap.add_argument("--sessions", type=int, default=8, help="--sessions of each deepeel (CPU use comes in bursts at the epoch barriers, so 4 explorations x 8 interleave on 16 cores)")
    ap.add_argument("--queue", type=int, default=60000, help="exploration states kept in RAM before the disk spool (0 = engine default 4096)")
    ap.add_argument("--only", default="")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--no-tas", action="store_true")
    ap.add_argument("--mapwin", action="store_true")
    ap.add_argument("--redo", action="store_true", help="also redo 'done' and 'capped' ROMs")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    RES = a.results
    os.makedirs(RES, exist_ok=True)
    import fcntl
    global _LOCK
    _LOCK = open(os.path.join(RES, ".batch.lock"), "w")      # one batch per results directory
    try:
        fcntl.flock(_LOCK, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        sys.exit("another romset_batch already runs on %s" % RES)
    os.makedirs(RES, exist_ok=True)
    os.makedirs(a.scratch, exist_ok=True)

    entries = []
    for root, _d, files in os.walk(a.roms):
        for f in files:
            if f.lower().endswith(".zip"):
                p = os.path.join(root, f)
                sub = os.path.relpath(root, a.roms)
                sub = "." if sub == "." else sub
                entries.append({"file": f, "src": p, "sub": slugify(sub) if sub != "." else "root",
                                "size": os.path.getsize(p)})
    entries.sort(key=lambda e: (e["size"], e["src"]))  # small ones first
    seen = {}
    for e in entries:
        s = slugify(os.path.splitext(e["file"])[0])
        n = seen.get((e["sub"], s), 0)
        seen[(e["sub"], s)] = n + 1
        e["slug"] = s if n == 0 else "%s-%d" % (s, n + 1)
        e["rel"] = os.path.join("roms", "gb", e["sub"], e["file"])
        e["repo_zip"] = os.path.join(a.repo_roms, e["sub"], e["file"])
    if a.only:
        keys = [k.strip().lower() for k in a.only.split("|") if k.strip()]   # several substrings separated by |
        entries = [e for e in entries if any(k in e["src"].lower() for k in keys)]
    idx_path = os.path.join(RES, "index.json")
    index = json.load(open(idx_path)) if os.path.exists(idx_path) else {}
    todo = [e for e in entries if index.get(e["sub"] + "/" + e["slug"]) not in
            (("done", "capped", "negligible_gain", "unsupported") if not a.redo else ())]
    if a.limit:
        todo = todo[:a.limit]
    print("%d zips, %d to do" % (len(entries), len(todo)))
    if a.dry_run:
        for e in todo[:20]:
            print(" ", e["sub"], e["slug"], e["size"])
        return
    cat = [] if a.no_tas else load_catalog(os.path.join(RES, "tasvideos_catalog.json"))
    metas = []
    if os.path.exists(os.path.join(RES, "ROMSET.json")):
        metas = json.load(open(os.path.join(RES, "ROMSET.json")))
    signal.signal(signal.SIGINT, lambda *_: STOP.set())
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())

    def work(e):
        if STOP.is_set():
            return
        os.makedirs(os.path.dirname(e["repo_zip"]), exist_ok=True)
        if not os.path.exists(e["repo_zip"]):
            shutil.copy2(e["src"], e["repo_zip"])
        m = process(e, cat, a)
        with LOCK:
            metas[:] = [x for x in metas if not (x["slug"] == m["slug"] and x["sub"] == m["sub"])] + [m]
            index[m["sub"] + "/" + m["slug"]] = m.get("status")
            json.dump(index, open(idx_path, "w"), indent=0)
        write_tables(list(metas))
        log("%s => %s (%s) known %s%% tas=%s" % (m["slug"], m.get("status"), m.get("stop"),
            m.get("map", {}).get("known_pct"), m.get("tas", {}).get("verdict")))

    with ThreadPoolExecutor(max_workers=a.jobs_roms) as ex:
        list(ex.map(work, todo))
    log("batch end: %d ROM(s) processed" % len(todo))


if __name__ == "__main__":
    main()

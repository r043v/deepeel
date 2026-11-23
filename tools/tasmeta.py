#!/usr/bin/env python3
"""Mandatory, shared TASVideos metadata lookup for every TAS tool (`deepeel tasdb emu`).

The emulator a movie was made and verified on (publication/submission `emulatorVersion`) is never guessed from the
movie header (a BK2 header can say 2.3.0 for a movie that syncs on 2.3.2 only): it comes from the TASVideos API,
through the local database `deepeel tasdb sync` builds.  A tool that replays a movie calls `require_emulator()`
and stops when the movie cannot be resolved.
"""
import io
import os
import subprocess
import sys
import zipfile
from pathlib import Path

DB = os.environ.get("DEEPEEL_TASDB", "/run/media/m/Windows/deepeel-data/tasdb")


def movie_frames(movie):
    """Input frames of a BK2 (rows of Input Log.txt)."""
    with zipfile.ZipFile(movie) as z, io.TextIOWrapper(z.open("Input Log.txt"), encoding="utf-8-sig") as log:
        return sum(1 for line in log if line.startswith("|"))


def require_emulator(deepeel, rom, movie):
    """Return the TASVideos emulator string (e.g. 'BizHawk 2.3.2') or exit with the reason."""
    frames = movie_frames(movie)
    r = subprocess.run([str(deepeel), "tasdb", "emu", str(rom), str(frames), "--dir", DB],
                       capture_output=True, text=True)
    hits = [dict(kv.split("=", 1) for kv in l.replace(" kind", "\x00kind").replace(" id", "\x00id").split("\x00"))
            for l in r.stdout.splitlines() if l.startswith("emulator=")]
    if r.returncode or not hits:
        sys.exit(f"tasmeta: TASVideos does not resolve this movie ({frames} frames): {r.stderr.strip()}\n"
                 f"  (run `deepeel tasdb sync --systems <SYS> --dir {DB}` first)")
    emus = {h["emulator"] for h in hits}
    if len(emus) != 1:
        sys.exit(f"tasmeta: ambiguous emulators {sorted(emus)} for {frames} frames")
    emu = emus.pop()
    print(f"tasmeta: {emu} ({', '.join(h['kind'] + ' ' + h['id'] for h in hits)}) from the TASVideos API", file=sys.stderr)
    return emu


def require_lib_for(emu, lib):
    """The emulator library must be built for the version TASVideos names."""
    version = emu.split()[-1]
    if version not in str(Path(lib).resolve()):
        sys.exit(f"tasmeta: the movie needs {emu} but --lib is {lib} (its path must name {version})")

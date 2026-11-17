import pathlib
import os
import subprocess
import sys
import tempfile

exe, rom = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    archive = pathlib.Path(tmp) / "ram.rt"
    env = dict(os.environ, DEEPEEL_GB_PATH_STEPS="64")
    subprocess.run([exe, "explore", rom, "--no-sessions", "--attract-frames", "0", "--no-auto-seed",
                    "--no-pointer-scan", "--no-closure", "--no-forced-ram-cut",
                    "--max-steps", "64", "--max-total", "1000", "--rtcode", str(archive)],
                   check=True, capture_output=True, timeout=30, env=env)
    rows = [line.split() for line in archive.read_text().splitlines() if line and not line.startswith("#")]
    entry = next(row for row in rows if row[0] == "c000" and row[1] == "00")
    assert entry[3] == "-1", entry

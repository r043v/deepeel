import pathlib
import re
import subprocess
import sys
import tempfile

exe, rom = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    base = [exe, "explore", rom, "--no-sessions", "--attract-frames", "0",
            "--no-pointer-scan", "--no-closure", "--bank-fork-cap", "1",
            "--state-dir", tmp, "--max-steps", "100"]
    first = subprocess.run(base + ["--max-total", "1000"], check=True,
                           capture_output=True, text=True, timeout=30)
    assert re.search(r"bank forks: 1 \(site cap hit [1-9]", first.stdout), first.stdout
    assert re.search(r"deferred [1-9]", first.stdout), first.stdout
    assert (pathlib.Path(tmp) / "frontier.bin").is_file()
    frontier = pathlib.Path(tmp) / "frontier.bin"
    prior = frontier.read_bytes()
    other_rom = pathlib.Path(tmp) / "other.gb"
    changed = bytearray(pathlib.Path(rom).read_bytes())
    changed[-1] ^= 1
    other_rom.write_bytes(changed)
    wrong = base.copy()
    wrong[2] = str(other_rom)
    mismatch = subprocess.run(wrong + ["--resume", "--max-total", "1000"], capture_output=True, text=True, timeout=30)
    assert mismatch.returncode != 0 and "identity does not match" in mismatch.stderr, mismatch.stderr
    assert frontier.read_bytes() == prior
    resumed = subprocess.run(base + ["--resume", "--max-total", "50000"], check=True,
                             capture_output=True, text=True, timeout=30)
    assert re.search(r"frontier: [1-9][0-9]* states resumed", resumed.stdout), resumed.stdout
    assert "rom-index 32768-32773" in resumed.stdout, resumed.stdout
    assert "rom-index 49152-49157" in resumed.stdout, resumed.stdout

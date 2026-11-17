import pathlib
import subprocess
import sys
import tempfile

exe, rom = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    archive = pathlib.Path(tmp) / "ram.rt"
    subprocess.run([exe, "explore", rom, "--no-sessions", "--attract-frames", "0",
                    "--no-pointer-scan", "--no-closure", "--max-total", "1000",
                    "--rtcode", str(archive)], check=True, capture_output=True, timeout=30)
    rows = [line.split() for line in archive.read_text().splitlines() if line and not line.startswith("#")]
    assert any(row[0] == "c000" and row[1] == "c9" and row[3] == "-1" for row in rows), rows

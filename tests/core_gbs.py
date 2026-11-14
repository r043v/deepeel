import json
import pathlib
import subprocess
import sys
import tempfile

# The GBS seeds are confirmed entry points: init and play must be executed (forced paths
# carrying the $2000 bank write), and a rip that matches no bank must fail loudly.
exe, fixture, rip = sys.argv[1:4]
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    report = d / "summary.json"
    cmd = [exe, "explore", fixture, "--core", "--gbs", rip, "--no-pointer-scan", "--max-total", "100000",
           "--attract-frames", "0", "--no-sessions", "--no-closure", "--summary", str(report)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    assert "gbs: init -> rom bank 1 $4100" in r.stdout, r.stdout
    assert "gbs: play -> rom bank 1 $4200" in r.stdout, r.stdout
    assert "gbs: rip bank 0 -> rom bank 1" in r.stdout, r.stdout
    starts = json.loads(report.read_text())["instruction_starts"]
    assert any(x["index"] == 0x4100 and x["forced"] for x in starts), starts
    assert any(x["index"] == 0x4200 and x["forced"] for x in starts), starts
    # a rip whose data matches no bank of the ROM: explicit --gbs failure, never silent
    bad = d / "bad.gbs"
    data = bytearray(pathlib.Path(rip).read_bytes())
    data[112:112 + 0x4000] = bytes(b ^ 0xFF for b in data[112:112 + 0x4000])
    bad.write_bytes(data)
    r = subprocess.run([exe, "explore", fixture, "--core", "--gbs", str(bad), "--no-pointer-scan",
                        "--max-total", "100000", "--attract-frames", "0", "--no-sessions", "--no-closure"],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode != 0, r.stdout
    assert "no seed could be mapped" in r.stderr, r.stderr

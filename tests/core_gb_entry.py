import json
import os
import pathlib
import subprocess
import sys
import tempfile

exe, fixture = sys.argv[1:]
with tempfile.TemporaryDirectory() as d:
    d = pathlib.Path(d)
    data = bytearray(pathlib.Path(fixture).read_bytes())
    # Bank 1 is never entered by the fixture's boot, which selects banks 2/3.
    data[0x4020:0x4023] = b"\x3e\x77\x76"
    data[0x0020:0x0023] = b"\x3e\x66\x76"
    data[0x8020:0x8023] = b"\x3e\x55\x76"
    rom = d / "entry.gb"
    rom.write_bytes(data)
    def run(entry=None):
        report = d / "summary.json"
        cmd = [exe, "explore", str(rom), "--core", "--max-total", "100000",
               "--attract-frames", "0", "--no-sessions", "--no-closure", "--no-pointer-scan", "--no-bank-forks", "--summary", str(report)]
        if entry is not None:
            cmd += ["--entry", entry]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        return result, report
    base, report = run()
    assert base.returncode == 0, base.stderr
    a = json.loads(report.read_text())
    assert "seeded entry points 0" in base.stdout
    assert not any(x["index"] == 0x4020 for x in a["instruction_starts"])
    seeded, report = run("1:4020")
    assert seeded.returncode == 0, seeded.stderr
    b = json.loads(report.read_text())
    assert any(x["index"] == 0x4020 and x["forced"] for x in b["instruction_starts"])
    for bank, index in [(0, 0x20), (2, 0x8020)]:
        seeded, report = run(f"{bank}:4020")
        assert seeded.returncode == 0, seeded.stderr
        starts = json.loads(report.read_text())["instruction_starts"]
        assert any(x["index"] == index and x["forced"] for x in starts)
    assert run("1:")[0].returncode != 0
    assert run("1:4020garbage")[0].returncode != 0
    assert run("65536:4020")[0].returncode != 0
    data[0x200:0x208] = b"\x00\x03\x10\x03\x20\x03\x30\x03"
    for target in (0x300, 0x310, 0x320, 0x330):
        data[target:target + 3] = b"\x3e\x42\x76"
    rom.write_bytes(data)
    env = dict(os.environ, DEEPEEL_GB_POINTER_SEEDS="1")
    capped = subprocess.run([exe, "explore", str(rom), "--core", "--max-total", "1000",
                             "--attract-frames", "0", "--no-sessions", "--no-closure",
                             "--summary", str(report)], capture_output=True, text=True, env=env, timeout=60)
    assert capped.returncode == 0, capped.stderr
    assert json.loads(report.read_text())["counters"]["seed_refused_cap"] > 0
    # MBC5's ninth bank bit must select bank 256 rather than bank 0.
    data.extend(bytes(0x800000 - len(data)))
    data[0x148] = 8
    data[0x400020:0x400023] = b"\x3e\x33\x76"
    rom.write_bytes(data)
    high, report = run("256:4020")
    assert high.returncode == 0, high.stderr
    assert any(x["index"] == 0x400020 and x["forced"]
               for x in json.loads(report.read_text())["instruction_starts"])

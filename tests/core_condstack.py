import json
import pathlib
import subprocess
import struct
import sys
import tempfile

exe, fixture_dir = sys.argv[1:]
fixtures = pathlib.Path(fixture_dir)
with tempfile.TemporaryDirectory() as tmp:
    for system in ("gb", "sms"):
        for kind in ("call", "ret"):
            rom = fixtures / ("fix_condstack_" + kind + "." + system)
            cmd = [exe, "explore", str(rom)] if system == "gb" else [exe, "z80", "explore", "sms", str(rom)]
            summary = pathlib.Path(tmp) / (system + kind + ".json")
            # --max-steps gives the SMS path the same 1000-step shape the GB
            # explorer already has: the loop path is cut and the queued fork
            # side always runs (the step wall alone left it to the workers' race)
            cmd += ["--no-sessions", "--attract-frames", "0", "--max-total", "5000", "--max-steps", "1000", "--no-closure", "--no-auto-seed", "--summary", str(summary)]
            if system == "gb":
                cmd += ["--no-pointer-scan"]
            subprocess.run(cmd, check=True, capture_output=True, timeout=30)
            starts = json.loads(summary.read_text())["instruction_starts"]
            target = 0x180 if kind == "call" else 0x153
            assert any(row["index"] == target and row["forced"] for row in starts), (system, kind, starts)
            if kind == "call":
                assert any(row["index"] == 0x181 and row["forced"] for row in starts), (system, "call return", starts)

    state = pathlib.Path(tmp) / "deferred"
    rom = fixtures / "fix_condstack_deferred.gb"
    base = [exe, "explore", str(rom), "--no-sessions", "--attract-frames", "0",
            "--no-pointer-scan", "--no-closure", "--no-auto-seed", "--no-cells", "--no-proj",
            "--fork-site-cap", "1", "--state-dir", str(state)]
    first = subprocess.run(base + ["--max-total", "500"], check=True, capture_output=True, text=True, timeout=30)
    data = (state / "frontier.bin").read_bytes()
    magic, state_size, count = struct.unpack_from("=III", data)
    assert magic == 0x31524644
    records = [struct.unpack_from("=IIIIH", data, 12 + i * (20 + state_size)) for i in range(count)]
    assert any(pc == 0x180 and flags & 0x100 for pc, flags, _, _, _ in records), (first.stdout, records)
    summary = pathlib.Path(tmp) / "resumed.json"
    subprocess.run(base + ["--resume", "--max-total", "5000", "--summary", str(summary)], check=True, capture_output=True, timeout=30)
    starts = json.loads(summary.read_text())["instruction_starts"]
    assert any(row["index"] == 0x181 and row["forced"] for row in starts), starts

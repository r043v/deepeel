"""An old disk state must run even while a looping RAM state keeps yielding."""
import json
import pathlib
import signal
import struct
import subprocess
import sys
import tempfile
import time

exe, fixture = sys.argv[1:3]
sega = len(sys.argv) > 3 and sys.argv[3] == "sms"
with tempfile.TemporaryDirectory(prefix="deepeel-spool-fair-") as tmp:
    root = pathlib.Path(tmp)
    rom = root / ("loop.sms" if sega else "loop.gb")
    data = bytearray(pathlib.Path(fixture).read_bytes())
    for pc in (0x200, 0x210, 0x220):
        data[pc:pc + 3] = bytes((0xC3, pc & 0xff, pc >> 8))
    entry = 0 if sega else 0x100
    data[entry:entry + 3] = b"\xc3\x00\x02"
    rom.write_bytes(data)
    state = root / "state"
    report = root / "result.json"
    command = [exe, "z80", "explore", "sms", str(rom)] if sega else [exe, "explore", str(rom)]
    base = command + ["--unbounded", "--state-dir", str(state),
            "--jobs", "1", "--queue-max", "1", "--max-steps", "8",
            "--no-sessions", "--attract-frames", "0",
            "--no-exact-state", "--no-nonsense", "--no-closure", "--summary", str(report)]
    base += ["--no-vector-seeds"] if sega else ["--no-pointer-scan"]

    def interrupt(args):
        p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(1)
        assert p.poll() is None, "exploration stopped before SIGINT"
        p.send_signal(signal.SIGINT)
        out, err = p.communicate(timeout=30)
        assert p.returncode == 0, err
        return out

    interrupt(base)
    initial = json.loads(report.read_text())
    assert not any(row["index"] == 0x220 for row in initial["instruction_starts"]), initial["instruction_starts"]
    frontier = state / "frontier.bin"
    blob = frontier.read_bytes()
    magic, state_size, count = struct.unpack_from("=III", blob)
    assert magic == 0x31524644 and count
    machine = blob[12 + 20:12 + 20 + state_size]
    assert len(machine) == state_size
    # The first record stays in RAM. The oldest disk record is the only path
    # to 0x220; later disk records loop at 0x210 and the RAM path at 0x200.
    pcs = [0x200, 0x220] + [0x210] * 40
    records = b"".join(struct.pack("=IIIIH2x", pc, 1, 0, 0, 0) + machine for pc in pcs)
    frontier.write_bytes(struct.pack("=III", magic, state_size, len(pcs)) + records)
    out = interrupt(base + ["--resume"])
    assert "spool: 0 states spilled" not in out, out
    result = json.loads(report.read_text())
    assert any(row["index"] == 0x220 and row["forced"] for row in result["instruction_starts"]), result["instruction_starts"]

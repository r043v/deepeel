"""A broken TMPDIR must not pin the frontier in RAM: the spool falls back to /tmp."""
import json
import pathlib
import signal
import subprocess
import sys
import tempfile
import time

exe, fixture = sys.argv[1:3]
with tempfile.TemporaryDirectory(prefix="deepeel-spool-fb-") as tmp:
    root = pathlib.Path(tmp)
    rom = root / "loop.gb"
    data = bytearray(pathlib.Path(fixture).read_bytes())
    data[0x100:0x103] = b"\xc3\x00\x02"
    rom.write_bytes(data)
    state = root / "state"
    report = root / "result.json"
    args = [exe, "explore", str(rom), "--unbounded", "--state-dir", str(state),
            "--jobs", "1", "--queue-max", "1", "--max-steps", "8",
            "--no-sessions", "--attract-frames", "0", "--no-exact-state",
            "--no-nonsense", "--no-closure", "--no-pointer-scan",
            "--summary", str(report)]
    env = { **__import__("os").environ, "TMPDIR": str(root / "no-such-dir") }
    p = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    time.sleep(1)
    assert p.poll() is None, "exploration stopped before SIGINT"
    p.send_signal(signal.SIGINT)
    out, err = p.communicate(timeout=30)
    assert p.returncode == 0, err
    assert "spool unavailable" not in out + err, "the spool must fall back to /tmp, not stay in RAM"
    # queue-max 1 with a looping path: the overflow really went through the disk
    assert "spool: 0 states spilled" not in out, out
    print("core_spool_fallback: ok")

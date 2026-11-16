"""Exercise SIGINT and resume of the generic kernel on its five ISAs."""
import os
import pathlib
import json
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time

# A8 (0.389.0): --unbounded keeps the platform path shape, so a tiny looping fixture
# exhausts within seconds (a legal stop: exhaustion or SIGINT are the only ones).
# A large explicit path budget keeps the loops running so SIGINT/resume are exercised.
os.environ["DEEPEEL_GB_PATH_STEPS"] = "1000000"

exe, rom = sys.argv[1:]
fixtures = pathlib.Path(rom).parent
cases = [("gb", ["explore", rom], ["--no-pointer-scan", "--no-nonsense", "--no-exact-state"]),
         ("gb_jobs", ["explore", rom], ["--no-pointer-scan", "--no-nonsense", "--jobs", "2", "--no-exact-state"]),
         ("z80", ["z80", "explore", "sms", str(fixtures / "fix_z80_branch.sms")], ["--max-steps", "100000000"]),
         ("6502", ["nes", "explore", str(fixtures / "fix_nes_branch.nes")], []),
         ("huc6280", ["pce", "explore", str(fixtures / "fix_pce_branch.pce")], []),
         ("68000", ["md", "explore", str(fixtures / "fix_md_fork.md")], [])]
exact_backends = {"68000", "6502", "huc6280"}   # backends with DpMachine.state_hash
with tempfile.TemporaryDirectory(prefix="deepeel-unbounded-") as directory:
    bad_state = pathlib.Path(directory) / "not-a-directory"
    bad_state.write_text("occupied")
    preserved_report = pathlib.Path(directory) / "existing.json"
    preserved_report.write_text("previous report")
    invalid = subprocess.run([exe, "explore", rom, "--core", "--unbounded",
                              "--state-dir", str(bad_state), "--no-pointer-scan",
                              "--summary", str(preserved_report)],
                             capture_output=True, text=True, timeout=30)
    assert invalid.returncode != 0 and "not a directory" in invalid.stderr
    assert preserved_report.read_text() == "previous report"
    for name, command, extra in cases:
        summary = pathlib.Path(directory) / (name + ".json")
        # No explicit state directory: the CLI should choose <summary>.state.
        cmd = [exe, *command, "--core", "--unbounded", *extra,
               "--no-sessions", "--no-solve", "--attract-frames", "0", "--visits-per-pc", "1",
               "--max-total", "1", "--max-time", "1", "--summary", str(summary)]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            time.sleep(2)
            if proc.poll() is not None and name in exact_backends:
                # E11/B5: a machine with an exact state hash ends a spin loop by itself (exhaustion), never at a cap
                stdout, stderr = proc.communicate()
                assert proc.returncode == 0 and "interrupted" not in stdout, (name, stdout, stderr)
                report = json.loads(summary.read_text())
                assert report["counters"].get("state_repeats", 0) > 0, f"{name}: ended without an exact-state cut"
                continue
            assert proc.poll() is None, f"{name}: unbounded run stopped at a configured cap"
            proc.send_signal(signal.SIGINT)
            stdout, stderr = proc.communicate(timeout=30)
            assert proc.returncode == 0, (name, stdout, stderr)
            assert "halted: interrupted" in stdout, name
            state = pathlib.Path(str(summary) + ".state")
            assert (state / "frontier.bin").is_file(), name
            assert (state / "coverage.bin").is_file(), name
            report = json.loads(summary.read_text())
            if name == "gb":
                assert report["counters"]["cooperative_yields"] > 0
                assert any(row["index"] == 0x156 and row["forced"] for row in report["instruction_starts"]), \
                    "pending forced side was starved by the infinite natural path"
            data = (state / "frontier.bin").read_bytes()
            magic, state_size, count = struct.unpack_from("=III", data)
            assert magic == 0x31524644 and len(data) == 12 + count * (20 + state_size), name   # P5 (0.389.0): the record carries the checkpoint epoch (SpoolHead 16 -> 20)
            records = [struct.unpack_from("=IIII", data, 12 + i * (16 + state_size)) for i in range(count)]
            assert any(not (record[1] & 1) for record in records), f"{name}: active natural path lost"
            resumed = subprocess.run([exe, *command, "--core", "--resume", *extra,
                                      "--state-dir", str(state), "--max-total", "100",
                                      "--no-sessions", "--attract-frames", "0"],
                                     capture_output=True, text=True, timeout=30)
            assert resumed.returncode == 0, (name, resumed.stderr)
            assert re.search(r"frontier: [1-9][0-9]* states resumed", resumed.stdout), name
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
